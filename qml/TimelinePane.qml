import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient
import "LeaveAdminCheck.js" as AdminCheck

Rectangle {
    id: root

    function startDownload(mediaKey, filename, mime, ask, surface) {
        if (!mediaKey || mediaKey.length === 0 || !app.downloads)
            return
        DownloadNotices.begin(mediaKey, surface || "")
        if (ask === true)
            app.downloads.saveAs(mediaKey, filename || "", mime || "")
        else
            app.downloads.download(mediaKey, filename || "", mime || "")
    }
    color: AppTheme.timelineBackground

    // The pane's ground when the theme gives `background` a gradient (Depth or
    // a custom theme); draws nothing otherwise. Under every child.
    // Not z: -1: a negative z draws a child BENEATH its parent's own fill,
    // so the gradient was painted and then covered (GUI check 2026-10-06).
    // First among the visual children instead, so everything else is above.
    ThemedSurface {
        id: paneGround
        anchors.fill: parent
        role: "timelineBackground"
        flatFill: false
        innerShadowEdge: "left"
    }

    signal newConversationRequested(string mode, var options)
    signal searchAllRoomsRequested(string query)
    signal discoverRoomsRequested()

    property var currentRoom: ({})
    property int spacesTick: 0
    Connections {
        target: (typeof app !== "undefined" && app) ? app.spaces : null
        ignoreUnknownSignals: true
        function onSpacesChanged() { root.spacesTick++ }
    }
    readonly property bool composerFocusable:
        app.currentScreen === 1 && app.loggedIn && !app.accountSwitching
        && currentRoom.membership === "joined" && currentRoom.isSpace !== true
        && !app.eventContext.active && !app.thread.active
        && messageComposer.visible && messageComposer.enabled
        && messageComposer.activeEditor().enabled
        && !messageComposer.activeEditor().readOnly

    readonly property bool threadPanelHoldsKeys:
        app.thread.active && threadPanel.visible
        && app.currentScreen === 1 && app.loggedIn && !app.accountSwitching
    readonly property bool composerShortcutAvailable:
        root.composerFocusable
        || (root.threadPanelHoldsKeys && threadPanel.composerFocusable)
    readonly property bool selectionShortcutAvailable:
        root.timelineFocusable
        || (root.threadPanelHoldsKeys && threadPanel.selectionFocusable)
    function focusComposerShortcut() {
        if (root.threadPanelHoldsKeys)
            return threadPanel.focusComposer()
        return root.focusComposerForTyping()
    }
    function focusTimelineShortcut() {
        if (root.threadPanelHoldsKeys)
            return threadPanel.focusMessageSelection(true)
        return root.focusTimeline()
    }
    function resumeTimelineShortcut() {
        if (root.threadPanelHoldsKeys)
            return threadPanel.focusMessageSelection(false)
        return root.resumeTimeline()
    }

    function focusComposerForTyping() {
        if (!root.composerFocusable)
            return false
        timeline.stopEntrySelection()
        if (root.keyboardSelectedEventId !== "")
            root.resumeSelectedEventId = root.keyboardSelectedEventId
        root.keyboardSelectedEventId = ""
        return messageComposer.focusForTyping()
    }

    property string keyboardSelectedEventId: ""
    property string resumeSelectedEventId: ""
    property string selectionRoomId: ""
    property string pendingReturnSelection: ""
    property bool restoredSelectionOutline: false
    function rememberSelection() {
        var id = root.keyboardSelectedEventId !== ""
                 ? root.keyboardSelectedEventId : root.resumeSelectedEventId
        if (id !== "" && root.selectionRoomId !== ""
                && root.selectionRoomId === app.currentRoomId)
            app.pagination.saveSelection(root.selectionRoomId, id)
    }
    onKeyboardSelectedEventIdChanged: rememberSelection()
    onResumeSelectedEventIdChanged: rememberSelection()
    function applyReturnSelection() {
        var id = root.pendingReturnSelection
        if (id === "" || !app.timeline || app.timeline.rowForStableId(id) < 0)
            return
        root.pendingReturnSelection = ""
        if (root.keyboardSelectedEventId !== ""
                || root.resumeSelectedEventId !== "")
            return
        root.resumeSelectedEventId = id
        if (app.settings.showSelectionOnReturn) {
            root.keyboardSelectedEventId = id
            root.restoredSelectionOutline = true
        }
    }

    Connections {
        target: app.timeline
        property string lastRoot: ""
        function onThreadPresentationChanged() {
            var rootId = app.timeline.threadFilterRootId || ""
            if (rootId !== "" && rootId !== lastRoot)
                Qt.callLater(function() {
                    if (root)
                        root.focusComposerForTyping()
                })
            lastRoot = rootId
        }
    }
    readonly property bool timelineFocusable:
        app.currentScreen === 1 && app.loggedIn && !app.accountSwitching
        && currentRoom.membership === "joined" && currentRoom.isSpace !== true
        && !app.eventContext.active && !app.thread.active && !root.invitePreview
        && timeline.visible && timeline.enabled && app.timeline.count > 0

    function focusTimeline() {
        if (!root.timelineFocusable)
            return false
        root.closeRowAnchoredSurfaces()
        messageComposer.cancelPendingFocus()
        timeline.forceActiveFocus()
        timeline.enterMessageNavigation()
        return true
    }

    function resumeTimeline() {
        if (!root.timelineFocusable)
            return false
        root.closeRowAnchoredSurfaces()
        messageComposer.cancelPendingFocus()
        timeline.stopEntrySelection()
        root.stopAutoscroll()
        timeline.cancelWheelMotion()
        timeline.noteReaderTookControl()
        timeline.forceActiveFocus()
        if (root.keyboardSelectedEventId === "")
            root.keyboardSelectedEventId = root.resumeSelectedEventId
        var row = timeline.viewRowForStableId(root.keyboardSelectedEventId)
        if (timeline.selectionDetached || !timeline.messageIntersectsViewport(row))
            timeline.selectVisibleMessage(1)
        return true
    }

    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            root.selectionRoomId = ""
            root.restoredSelectionOutline = false
            root.keyboardSelectedEventId = ""
            root.resumeSelectedEventId = ""
            timeline.selectionDetached = false
            timeline.stopEntrySelection()
            root.selectionRoomId = app.currentRoomId
            root.pendingReturnSelection = app.currentRoomId !== ""
                    ? app.pagination.savedSelection(app.currentRoomId) : ""
            root.applyReturnSelection()
        }
    }
    Connections {
        target: app.timeline
        function onCountChanged() {
            if (root.keyboardSelectedEventId !== ""
                    && app.timeline.rowForStableId(root.keyboardSelectedEventId) < 0)
                root.keyboardSelectedEventId = ""
            if (root.resumeSelectedEventId !== ""
                    && app.timeline.rowForStableId(root.resumeSelectedEventId) < 0)
                root.resumeSelectedEventId = ""
            root.applyReturnSelection()
        }
    }
    readonly property bool inviteAvatarWithheld:
        root.invitePreview
        && !(app.accountPrefs && app.accountPrefs.inviteAvatarsAllowed)
    readonly property bool invitePreview:
        app.currentRoomId !== ""
        && (root.currentRoom.membership === "invited"
            || (!!app.roomInfo && app.roomInfo.roomId === app.currentRoomId
                && (app.roomInfo.ownMembership === "invite"
                    || app.roomInfo.ownMembership === "invited")))
    property bool openedWithUnread: false
    property bool firstUnreadHuntFailed: false
    property bool firstUnreadNoteShown: false
    onFirstUnreadHuntFailedChanged: {
        firstUnreadNoteShown = firstUnreadHuntFailed
        if (firstUnreadHuntFailed)
            firstUnreadNoteTimer.restart()
        else
            firstUnreadNoteTimer.stop()
    }
    function snapshotOpenedUnread() {
        openedWithUnread = app.currentRoomId !== ""
                           && (currentRoom.unreadCount || 0) > 0
        firstUnreadHuntFailed = false
        noteLoadedReadMarker()
    }
    function noteLoadedReadMarker() {
        if (openedWithUnread && app.timeline
                && app.timeline.roomId === app.currentRoomId
                && app.timeline.firstUnreadRow >= 0)
            openedWithUnread = false
    }
    Connections {
        target: app.timeline
        // firstUnreadRow NOTIFYs on countChanged; roomId covers a model that
        // loads its rows before it names the room.
        function onCountChanged() { root.noteLoadedReadMarker() }
        function onRoomIdChanged() { root.noteLoadedReadMarker() }
    }
    Connections {
        target: app.readReceipts
        function onFullyReadMarkerMoved(roomId) {
            if (roomId === app.currentRoomId)
                root.openedWithUnread = false
        }
    }
    property bool infoOpen: false
    property bool searchOpen: false
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen
    readonly property bool threadInMainView:
        app.thread.active && app.settings.threadPlacement === 1
    readonly property bool callStageOwnsColumn:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId
        && !(app.groupCall.stageState
             && app.groupCall.stageState.pictureInPicture)

    property bool callPanelCollapsed: false
    property real callPanelUserHeight: -1
    property real callPanelDragBase: 0
    readonly property real callPanelCollapsedHeight: 64
    readonly property bool callPanelHasVideo:
        callStageHost.active && callStageHost.item
        ? !callStageHost.item.voiceOnly : false
    readonly property real callPanelAutoHeight:
        clampCallPanelHeight(root.height * (root.callPanelHasVideo ? 0.70 : 0.40))
    readonly property real callPanelHeight:
        root.callPanelCollapsed
        ? root.callPanelCollapsedHeight
        : (root.callPanelUserHeight < 0
           ? root.callPanelAutoHeight
           : clampCallPanelHeight(root.callPanelUserHeight))

    readonly property real callPanelVideoFloor:
        callStageHost.active && callStageHost.item
        ? callStageHost.item.minimumUsefulHeight : 220

    function clampCallPanelHeight(value) {
        var floor = root.callPanelHasVideo
            ? Math.min(root.callPanelVideoFloor,
                       Math.max(64, root.height * 0.6))
            : Math.min(220, Math.max(64, root.height * 0.45))
        var ceiling = Math.max(floor, root.height * 0.75)
        var reserve = roomHeaderBand.height + timelineCallHeaderBar.height + 9
                      + (messageComposer.visible ? messageComposer.height : 0)
                      + 64
        var room = Math.max(64, root.height - reserve)
        floor = Math.min(floor, room)
        ceiling = Math.min(ceiling, room)
        return Math.max(floor, Math.min(value, ceiling))
    }

    onCallPanelHasVideoChanged: {
        if (!root.callPanelHasVideo || root.callPanelUserHeight < 0)
            return
        var target = clampCallPanelHeight(root.height * 0.70)
        if (root.callPanelUserHeight < target)
            root.callPanelUserHeight = target
    }

    onCallStageOwnsColumnChanged: {
        if (!root.callStageOwnsColumn)
            root.callPanelCollapsed = false
    }
    readonly property string rightPanelState:
        threadSurfaceOpen ? "thread"
        : (searchOpen ? "search" : (infoOpen ? "info" : "none"))

    function refreshCurrentRoom() {
        currentRoom = app.currentRoomId === ""
                      ? ({})
                      : app.roomList.findRoom(app.currentRoomId)
    }

    // Cancels a middle-click autoscroll. Called from every site that moves the
    // view (wheel, keyboard page, jump to latest, reply jump) so the two never
    // write contentY on alternate frames.
    //
    // Not folded into timeline.cancelWheelMotion(): the scroller calls that
    // when its gesture starts, which would end every autoscroll on its first
    // frame.
    function stopAutoscroll() {
        if (middleClickScroller.active)
            middleClickScroller.stop()
    }

    function focusComposer() {
        if (!messageComposer.visible || !messageComposer.enabled)
            return false
        messageComposer.focusEditor()
        return true
    }

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

    function rememberMemberPanel() {
        app.settings.memberPanelOpen =
            root.infoOpen && infoPanel.section === "people"
    }
    function restoreMemberPanel() {
        if (!app.settings.memberPanelOpen || app.currentRoomId === ""
                || !app.roomInfo.supported)
            return
        if (root.width > 0 && !sideSurface.infoBeside)
            return
        infoPanel.openForRoom(app.currentRoomId)
        infoPanel.section = "people"
        infoOpen = true
    }

    function toggleRoomInfo() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        if (infoOpen) {
            infoOpen = false
            rememberMemberPanel()
            return
        }
        infoPanel.openForRoom(app.currentRoomId)
        infoOpen = true
        rememberMemberPanel()
    }

    function openRoomOverview() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        infoPanel.openForRoom(app.currentRoomId)
        infoPanel.section = "overview"
        infoOpen = true
        rememberMemberPanel()
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

    function toggleThreadSurface() {
        if (threadSurfaceOpen) {
            if (app.thread.active)
                app.thread.close()
            app.thread.closeList()
        } else {
            app.thread.openList(app.currentRoomId)
        }
    }

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
        rememberMemberPanel()
    }

    function toggleMemberPanel() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        if (infoOpen && infoPanel.section === "people") {
            infoOpen = false
            rememberMemberPanel()
            return
        }
        infoPanel.openForRoom(app.currentRoomId)
        infoPanel.section = "people"
        infoOpen = true
        rememberMemberPanel()
    }

    Connections {
        target: app
        property bool infoOpenBeforeSettings: false
        property string infoSectionBeforeSettings: ""
        function onCurrentScreenChanged() {
            if (app.currentScreen === 2) {
                infoOpenBeforeSettings = root.infoOpen
                infoSectionBeforeSettings = root.infoOpen ? infoPanel.section : ""
                root.infoOpen = false
                root.searchOpen = false
            } else if (app.currentScreen === 1 && infoOpenBeforeSettings
                       && app.currentRoomId !== "") {
                infoOpenBeforeSettings = false
                if (infoSectionBeforeSettings.length > 0)
                    infoPanel.section = infoSectionBeforeSettings
                root.infoOpen = true
            }
        }
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            snapshotOpenedUnread()
            if (root.findOpen) {
                root.findOpen = false
                root.findHistoryMode = false
                app.messageSearch.query = ""
                findField.text = ""
            }
            timeline.cancelWheelMotion()
            root.stopAutoscroll()
            timeline.firstUnreadSeen = false
            timeline.firstUnreadSeenId = ""
            timeline.pinnedActionsKey = ""
            timeline.viewAnchorId = ""
            timeline.viewAnchorOffset = 0
            timeline.viewAnchorLastY = 0
            root.infoOpen = false
            root.searchOpen = false
            root.restoreMemberPanel()
        }
    }

    property bool findOpen: false
    property bool findHistoryMode: false
    readonly property bool serverSearchAvailable:
        app.messageSearch.supported
        && root.currentRoom.encryptionKnown === true
        && root.currentRoom.encrypted !== true
    readonly property bool findHistoryAvailable:
        app.messageSearch.localAvailable || root.serverSearchAvailable

    function openFind() {
        if (app.currentRoomId === "") return
        root.searchOpen = false
        root.findOpen = true
        root.findScrolledEventId = ""
        app.timeline.beginSearch(findField.text)
        findField.forceActiveFocus()
        findField.selectAll()
        findFocusTimer.restart()
    }
    Timer {
        id: findFocusTimer
        interval: 0
        repeat: false
        onTriggered: {
            if (root.findOpen && findField.visible && !findField.activeFocus)
                findField.forceActiveFocus()
        }
    }
    property string findScrolledEventId: ""
    function stepFind(forward) {
        root.findScrolledEventId = ""
        if (forward)
            app.timeline.searchNext()
        else
            app.timeline.searchPrev()
    }
    function closeFind() {
        root.findOpen = false
        root.findHistoryMode = false
        app.messageSearch.query = ""
        app.timeline.endSearch()
        timeline.forceActiveFocus()
    }
    function scrollToSearchMatch() {
        var eventId = app.timeline.searchCurrentEventId
        root.findScrolledEventId = eventId
        if (eventId === "") return
        timeline.diagNavigationShape("find-before")
        timeline.releasePendingRows()
        var sourceRow = app.timeline.rowForStableId(eventId)
        if (sourceRow < 0) {
            console.warn("timeline find match not in the loaded rows"
                         + " srcRows=" + app.timeline.count
                         + " matches=" + app.timeline.searchResultCount)
            return
        }
        timeline.cancelWheelMotion()
        root.stopAutoscroll()
        timeline.stickToBottom = false
        // Through the navigation landing rather than positioning now:
        // releasePendingRows() can build hundreds of rows in this turn, and a
        // row built this turn reads y == 0 until the Column's next pass, so a
        // direct position measured stale geometry (break-it B021). The landing
        // holds the match by stable id, waits for a layout pass covering the
        // new row set, places it a third of the way down like every other
        // jump, and sets follow-latest from where it lands.
        timeline.beginNavigationLanding(sourceRow, 0, true)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.find")]
        }
        enabled: app.currentRoomId !== ""
        onActivated: root.openFind()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.findNext")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
                 && app.timeline.searchActive
        onActivated: { if (!root.findHistoryMode) root.stepFind(true) }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.findPrevious")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
                 && app.timeline.searchActive
        onActivated: { if (!root.findHistoryMode) root.stepFind(false) }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.togglePeople")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
        onActivated: root.toggleMemberPanel()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.togglePinned")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
        onActivated: root.togglePinnedPanel()
    }
    Connections {
        target: app.timeline
        function onSearchChanged() {
            if (root.findOpen && app.timeline.searchCurrentEventId
                                 !== root.findScrolledEventId)
                root.scrollToSearchMatch()
        }
    }

    Shortcut {
        sequence: "Escape"
        // Two enabled Shortcuts on one sequence are ambiguous and Qt fires
        // neither, so exclude the owners explicitly: an active autoscroll and a
        // forward selection have their own Escape. Also gated to this screen,
        // since Settings keeps MainScreen loaded and Shortcuts match by window.
        enabled: app.currentScreen === 1
                 && !timeline.emojiPickerOpen && !middleClickScroller.active
                 && !app.forward.selecting
                 && !app.eventContext.active
                 && (root.findOpen || root.infoOpen || root.searchOpen
                     || root.threadSurfaceOpen
                     || timeline.pinnedActionsKey !== "")
        onActivated: {
            if (root.findOpen)
                root.closeFind()
            // A pending edit in the main message box is cancelled before any
            // panel closes: this Shortcut is matched before the message
            // box's own Escape handler, so without it the panel closed and
            // the edit stayed armed for the next Return (C46 F03).
            else if (app.composer.isEditing)
                app.composer.cancelReplyOrEdit()
            else if (root.infoOpen) {
                root.infoOpen = false
                root.rememberMemberPanel()
            } else if (root.searchOpen)
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
    Component.onCompleted: {
        refreshCurrentRoom()
        snapshotOpenedUnread()
        root.restoreMemberPanel()
    }

    ImageViewerOverlay {
        id: imageViewer
        onOpened: timeline.claimTransientInteraction("viewer")
        onClosed: timeline.releaseTransientInteraction("viewer")
    }

    AnchoredPopup {
        id: receiptListPopover
        objectName: "receiptListPopover"
        property var readers: []
        property int totalOthers: 0
        readonly property int unnamed:
            Math.max(0, totalOthers - readers.length)
        modal: true
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: AppTheme.spacing12
        preferAbove: true
        onOpened: timeline.claimTransientInteraction("readers")
        onClosed: timeline.releaseTransientInteraction("readers")
        width: Math.min(280, maxWidth)
        height: {
            var want = (contentItem ? contentItem.implicitHeight : 0)
                       + topPadding + bottomPadding
            var cap = Math.min(maxHeight,
                               overlayItem ? overlayItem.height * 0.5 : 400)
            return Math.max(Math.min(want, cap), 96)
        }
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.menuRadius + 6
        }
        function formatReadTime(tsMs) {
            if (!tsMs || tsMs <= 0)
                return ""
            var d = new Date(tsMs)
            var now = new Date()
            var startOfToday = new Date(now.getFullYear(), now.getMonth(),
                                        now.getDate())
            if (d >= startOfToday)
                return Qt.formatTime(d, app.settings.clockTimeFormat)
            if (startOfToday - d < 6 * 86400000)
                return Qt.locale(AppLocale.name).toString(d, "ddd hh:mm")
            return Qt.locale(AppLocale.name).toString(d, "MMM d, hh:mm")
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            Label {
                // Branch the plural explicitly: without a loaded translation
                // the %n form renders "(s)" literally.
                text: receiptListPopover.totalOthers === 1
                      ? qsTr("Seen by 1 person")
                      : qsTr("Seen by %1 people")
                            .arg(receiptListPopover.totalOthers)
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                font.weight: AppTheme.weightBold
            }
            ListView {
                objectName: "receiptReaderList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: contentHeight
                clip: true
                spacing: 2
                model: receiptListPopover.readers
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Item {
                    id: readerDelegate
                    required property var modelData
                    width: ListView.view.width
                    height: readerRow.implicitHeight + AppTheme.spacing8
                    objectName: "receiptReaderRow"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: readerDelegate.modelData.displayName
                                     || readerDelegate.modelData.userId
                    Accessible.onPressAction: readerDelegate.openProfile()
                    function openProfile() {
                        var uid = readerDelegate.modelData.userId || ""
                        if (uid.length === 0)
                            return
                        var tl = readerDelegate.mapToItem(Overlay.overlay, 0, 0)
                        var anchor = { x: tl.x, y: tl.y,
                                       width: readerDelegate.width,
                                       height: readerDelegate.height }
                        receiptListPopover.close()
                        timeline.openSenderProfile({
                            userId: uid,
                            displayName: readerDelegate.modelData.displayName
                                         || "",
                            avatarUrl: readerDelegate.modelData.avatarMxc
                                       || ""
                        }, anchor)
                    }
                    Keys.onReturnPressed: openProfile()
                    Keys.onEnterPressed: openProfile()
                    Keys.onSpacePressed: openProfile()
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: readerDelegate.openProfile()
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppTheme.radiusMd
                        color: AppTheme.stormSelection
                        visible: readerHover.hovered
                                 || readerDelegate.activeFocus
                        border.width: readerDelegate.activeFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                    }
                    HoverHandler {
                        id: readerHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    RowLayout {
                        id: readerRow
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: AppTheme.spacing4
                        anchors.rightMargin: AppTheme.spacing4
                        spacing: AppTheme.spacing8
                        Avatar {
                            size: 28
                            onScreen: true
                            name: readerDelegate.modelData.displayName
                                  || readerDelegate.modelData.userId
                            mxc: readerDelegate.modelData.avatarMxc || ""
                            colorKey: readerDelegate.modelData.userId || ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                text: readerDelegate.modelData
                                          .displayName
                                      || readerDelegate.modelData.userId
                                textFormat: Text.PlainText
                                color: AppTheme.stormText
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                font.weight: AppTheme.weightMedium
                                elide: Label.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: receiptListPopover.formatReadTime(
                                          readerDelegate.modelData.tsMs)
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                            }
                        }
                    }
                }
                footer: Label {
                    visible: receiptListPopover.unnamed > 0
                    height: visible ? implicitHeight : 0
                    width: ListView.view ? ListView.view.width : 0
                    text: qsTr("…and %n more (names not loaded)", "",
                               receiptListPopover.unnamed)
                    color: AppTheme.stormTextMuted
                    font.family: AppTheme.menuFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    topPadding: 4
                }
            }
        }
    }

    EmojiPicker {
        id: sharedReactionPicker
        objectName: "sharedReactionPicker"
        mode: "reaction"
        property string targetEventId: ""
        onOpened: {
            timeline.emojiPickerOpen = true
            timeline.claimTransientInteraction("picker")
        }
        onClosed: {
            timeline.emojiPickerOpen = false
            timeline.releaseTransientInteraction("tone")
            timeline.releaseTransientInteraction("picker")
            targetEventId = ""
        }
        onToneOpened: timeline.claimTransientInteraction("tone")
        onToneClosed: timeline.releaseTransientInteraction("tone", "picker")
        onEmojiChosen: (emoji) => {
            if (targetEventId !== "")
                app.composer.reactTo(targetEventId, emoji)
        }
    }
    MemberProfilePopover {
        id: senderProfilePopover
        objectName: "senderProfilePopover"
        parent: Overlay.overlay
        onOpened: timeline.claimTransientInteraction("profile")
        onClosed: timeline.releaseTransientInteraction("profile")
    }
    InvitePeopleDialog {
        id: roomInviteDialog
        objectName: "roomInviteDialog"
        parent: Overlay.overlay
    }

    JumpToDateDialog {
        id: jumpToDateDialog
        parent: Overlay.overlay
    }

    ExportRoomDialog {
        id: exportRoomDialog
        objectName: "exportRoomDialogHost"
        parent: Overlay.overlay
    }
    function openExportRoom() {
        exportRoomDialog.openDialog()
    }

    function closeRowAnchoredSurfaces() {
        sharedReactionPicker.close()
        senderProfilePopover.close()
        receiptListPopover.close()
        imageViewer.close()
        timeline.transientInteractionOwner = ""
    }

    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            root.closeRowAnchoredSurfaces()
        }
        function onAccountSwitchingChanged() {
            if (app.accountSwitching) {
                imageViewer.close()
                receiptListPopover.close()
                root.stopAutoscroll()
            }
        }
    }
    Connections {
        target: app.about
        function onEventCacheWasCleared() {
            if (app.currentRoomId !== "")
                app.reloadCurrentRoomTimeline()
            if (app.thread.active)
                app.thread.reopen()
        }
    }
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenMessageContextMenu() {
            var tries = Math.min(timeline.count, 40)
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
            timeline.openSenderProfile({
                userId: "@maya:lightning.example",
                displayName: "Maya Chen",
                membership: "joined",
                role: "",
                avatarUrl: "mxc://lightning.example/avatar-maya",
                isOwn: false
            })
        }
        function onDemoOpenFindBar(query) {
            root.openFind()
            findField.text = query
        }
        function onDemoOpenFindBarHistory(query) {
            root.openFind()
            findField.text = query
            root.findHistoryMode = true
            app.messageSearch.source =
                app.messageSearch.localAvailable ? "local" : "server"
            app.timeline.endSearch()
            app.messageSearch.roomId = app.currentRoomId
            app.messageSearch.filters = ({})
            app.messageSearch.query = query
        }
        function onDemoOpenRoomInfo(section) {
            root.toggleRoomInfo()
            if (section !== "" && root.infoOpen)
                infoPanel.section = section
        }
    }
    Connections {
        target: app.mediaBridge
        function onSaveCancelled(mediaKey) {
            timeline.cancelledKey = mediaKey
        }
        function onSaveFinished(ok, message, mediaKey) {
            timeline.noteSaveFinished(ok, mediaKey)
        }
    }
    Connections {
        target: app.gif.starredStore
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
        id: roomColumn
        objectName: "roomColumn"
        visible: !((root.threadSurfaceOpen && !sideSurface.threadBeside)
                   || (root.searchOpen && !sideSurface.beside)
                   || sideSurface.infoTakesPane)
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 320
        spacing: 0

        Rectangle {
            id: roomHeaderBand
            objectName: "roomHeaderBand"
            Layout.fillWidth: true
            implicitHeight: AppTheme.headerBandHeight
            // The timeline paints after this band, so anything drawn outside it
            // would sit over the messages and be unclickable (e.g. a topic with
            // newlines).
            clip: true
            color: AppTheme.roomHeader
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing20
                anchors.rightMargin: AppTheme.spacing20
                spacing: AppTheme.spacing12

                readonly property real identityFloor:
                    Math.round(AppTheme.scaled(AppTheme.textTitle) * 9)
                    + AppTheme.spacingS + 14
                readonly property int actionSlots: {
                    if (width <= 0)
                        return 99
                    var step = 34 + AppTheme.spacing6
                    var budget = width
                                 - (roomHeaderAvatar.visible
                                    ? roomHeaderAvatar.size + spacing : 0)
                                 - 2 * spacing
                                 - identityFloor
                    return Math.max(
                        0, Math.floor((budget + AppTheme.spacing6) / step))
                }

                readonly property string dmPeerId:
                    root.currentRoom.isDirect === true
                    && (root.currentRoom.identityColorKey || "").charAt(0) === "@"
                    ? root.currentRoom.identityColorKey : ""
                function openPeerProfile() {
                    if (dmPeerId === "")
                        return
                    senderProfilePopover.openFor({
                        userId: dmPeerId,
                        displayName: "",
                        avatarUrl: "",
                        isOwn: app.accounts
                               && dmPeerId === app.accounts.activeUserId
                    }, header)
                }

                Avatar {
                    id: roomHeaderAvatar
                    objectName: "roomHeaderAvatar"
                    visible: app.currentRoomId !== ""
                    size: 34
                    squareRadius: 9
                    name: root.currentRoom.name || app.currentRoomId
                    mxc: root.inviteAvatarWithheld
                         ? "" : (root.currentRoom.avatarUrl || "")
                    colorKey: root.currentRoom.identityColorKey
                              || app.currentRoomId
                    circle: root.currentRoom.isDirect === true

                    // A DM's avatar opens the peer's profile. WithinBounds
                    // takes the exclusive grab.
                    TapHandler {
                        objectName: "roomHeaderAvatarTap"
                        enabled: header.dmPeerId !== ""
                        gesturePolicy: TapHandler.WithinBounds
                        acceptedButtons: Qt.LeftButton
                        onTapped: header.openPeerProfile()
                    }
                    HoverHandler {
                        enabled: header.dmPeerId !== ""
                        cursorShape: Qt.PointingHandCursor
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("View profile")
                    Accessible.ignored: header.dmPeerId === ""
                    Accessible.onPressAction: header.openPeerProfile()

                    PresenceDot {
                        objectName: "roomHeaderPresenceDot"
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        dotSize: 12
                        ring: roomHeaderBand.color
                        hoverStatus: true
                        userId: header.dmPeerId
                    }
                }
                ColumnLayout {
                    objectName: "roomHeaderIdentity"
                    spacing: 2
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.maximumWidth: 16777215
                    Layout.minimumWidth: 0
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Label {
                            id: roomHeaderTitle
                            objectName: "roomHeaderTitle"
                            // Untrusted text: never markup.
                            textFormat: Text.PlainText
                            text: {
                                if (root.currentRoom.name)
                                    return root.currentRoom.name
                                if (app.currentRoomId !== "")
                                    return app.currentRoomId
                                var _dep = root.spacesTick
                                if (app.spaces
                                        && app.spaces.activeSpaceId.length > 0
                                        && app.spaces.activeSpaceId.charAt(0) === "!")
                                    return app.spaces.spaceName(
                                        app.spaces.activeSpaceId) || qsTr("Space")
                                return qsTr("Home")
                            }
                            color: AppTheme.roomHeaderTitle
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                            font.weight: AppTheme.weightBold
                            elide: Label.ElideRight
                            maximumLineCount: 1
                            Layout.fillWidth: true
                            Layout.maximumWidth: Math.ceil(implicitWidth)

                            TapHandler {
                                objectName: "roomHeaderTitleTap"
                                enabled: header.dmPeerId !== ""
                                gesturePolicy: TapHandler.WithinBounds
                                acceptedButtons: Qt.LeftButton
                                onTapped: header.openPeerProfile()
                            }
                            HoverHandler {
                                enabled: header.dmPeerId !== ""
                                cursorShape: Qt.PointingHandCursor
                            }
                            Accessible.role: header.dmPeerId !== ""
                                             ? Accessible.Button
                                             : Accessible.StaticText
                            Accessible.name: text
                            Accessible.description: header.dmPeerId !== ""
                                                    ? qsTr("View profile") : ""
                            Accessible.onPressAction: header.openPeerProfile()
                        }
                        Icon {
                            id: encryptionLock
                            visible: root.currentRoom.encrypted === true
                            name: "lock"
                            size: 14
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
                        text: (root.currentRoom.topic || "")
                                  .replace(/\s+/g, " ").trim()
                        // Untrusted server text. Label defaults to AutoText, so
                        // a topic with an <img> tag would make every member
                        // fetch an attacker-chosen URL outside the media
                        // bridge.
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        visible: text.length > 0
                        Layout.fillWidth: true
                        elide: Label.ElideRight
                        maximumLineCount: 1
                    }
                    Item { Layout.fillHeight: true }
                    TapHandler {
                        objectName: "roomHeaderIdentityTap"
                        enabled: app.currentRoomId !== "" && app.roomInfo.supported
                        onTapped: (eventPoint) => {
                            if (header.dmPeerId !== ""
                                    && roomHeaderTitle.contains(
                                        roomHeaderTitle.mapFromItem(
                                            parent, eventPoint.position)))
                                return
                            root.toggleRoomInfo()
                        }
                    }
                }
                Item { objectName: "roomHeaderSpacer"; Layout.fillWidth: true }
                RowLayout {
                    id: roomHeaderActions
                    objectName: "roomHeaderActions"
                    spacing: AppTheme.spacing6
                    Layout.fillWidth: false

                    readonly property var foldOrder: [
                        "roomInfoButton", "pinnedMessagesButton",
                        "memberPanelButton", "timelineSearchButton",
                        "threadRepliesToggleButton",
                        "threadsViewButton", "startVoiceCallButton"]
                    readonly property var actionButtons: [
                        startVoiceCallButton, pinnedMessagesButton,
                        threadRepliesToggleButton,
                        threadsViewButton, timelineSearchButton,
                        memberPanelButton, roomInfoButton]
                    readonly property var foldedActions: {
                        var live = []
                        for (var i = 0; i < actionButtons.length; ++i)
                            if (actionButtons[i].available)
                                live.push(actionButtons[i].objectName)
                        var keep = Math.max(0, header.actionSlots - 1)
                        if (live.length <= keep)
                            return []
                        var folded = []
                        for (var j = 0;
                             j < foldOrder.length
                             && live.length - folded.length > keep; ++j)
                            if (live.indexOf(foldOrder[j]) >= 0)
                                folded.push(foldOrder[j])
                        return folded
                    }

                    IconButton {
                        id: startVoiceCallButton
                        objectName: "startVoiceCallButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== ""
                            && app.canStartCall(app.currentRoomId)
                            && app.callGateRevision >= 0
                            && !app.groupCall.active
                            && (app.calls.state === CallController.Idle
                                || app.calls.state === CallController.Ended)
                        enabled: true
                        iconName: "call"
                        property string actionLabel: qsTr("Start a voice call")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Start a voice call")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: app.startCall(app.currentRoomId, false)
                    }
                    AbstractButton {
                        id: callFormatChevron
                        objectName: "callFormatChevron"
                        visible: startVoiceCallButton.visible
                        implicitWidth: 16
                        implicitHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: -AppTheme.spacing4
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        Accessible.role: Accessible.ButtonMenu
                        Accessible.name: qsTr("Call options")
                        ToolTip.text: qsTr("Call options")
                        ToolTip.visible: hovered && !callFormatMenu.visible
                        ToolTip.delay: 500
                        background: Rectangle {
                            radius: AppTheme.radiusSm
                            color: callFormatChevron.hovered
                                   || callFormatChevron.activeFocus
                                   ? AppTheme.hover : "transparent"
                        }
                        contentItem: Icon {
                            name: "expand_more"
                            size: 14
                            color: AppTheme.stormTextMuted
                        }
                        onClicked: callFormatMenu.popup(
                            callFormatChevron, 0, callFormatChevron.height)
                    }
                    IconButton {
                        id: pinnedMessagesButton
                        objectName: "pinnedMessagesButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== ""
                            && app.roomInfo.supported
                            && app.pinned
                            && app.pinned.supported
                            && app.pinned.roomId === app.currentRoomId
                            && app.pinned.total > 0
                        iconName: "push_pin"
                        active: root.infoOpen && infoPanel.section === "pinned"
                        property string actionLabel: qsTr("Pinned messages")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Pinned messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.togglePinnedPanel()
                    }
                    IconButton {
                        id: threadRepliesToggleButton
                        objectName: "threadRepliesToggleButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== "" && app.thread.supported
                            && app.timeline.threadRepliesInline === true
                        readonly property bool repliesHidden:
                            app.settings.hideThreadReplies === true
                        iconName: repliesHidden ? "visibility_off" : "forum"
                        active: repliesHidden
                        property string actionLabel: repliesHidden
                            ? qsTr("Show thread replies")
                            : qsTr("Hide thread replies")
                        Accessible.name: actionLabel
                        ToolTip.text: actionLabel
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: app.settings.hideThreadReplies = !repliesHidden
                    }
                    IconButton {
                        id: threadsViewButton
                        objectName: "threadsViewButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== "" && app.thread.supported
                        iconName: "forum"
                        active: root.threadSurfaceOpen
                        readonly property int threadUnread:
                            app.thread.roomThreadUnread
                        property string actionLabel: threadUnread === 2
                            ? qsTr("Threads, mention in a thread")
                            : threadUnread === 1
                              ? qsTr("Threads, unread in a thread")
                              : qsTr("Threads")
                        Accessible.name: actionLabel
                        ToolTip.text: threadUnread === 2
                            ? qsTr("Mention in a thread")
                            : threadUnread === 1
                              ? qsTr("Unread in a thread")
                              : qsTr("Threads in this room")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleThreadSurface()
                        Rectangle {
                            objectName: "threadsUnreadDot"
                            visible: threadsViewButton.threadUnread > 0
                            width: 11
                            height: 11
                            radius: 5.5
                            x: parent.width - width - 3
                            y: 3
                            color: AppTheme.background
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 1
                                radius: height / 2
                                color: threadsViewButton.threadUnread === 2
                                       ? AppTheme.mentionDot : AppTheme.unreadDot
                                border.width: AppTheme.badgeDotRingWidth
                                border.color: threadsViewButton.threadUnread === 2
                                              ? AppTheme.mentionDotRing
                                              : AppTheme.unreadDotRing
                            }
                        }
                    }
                    IconButton {
                        id: timelineSearchButton
                        objectName: "timelineSearchButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available: app.currentRoomId !== ""
                        iconName: "search"
                        active: root.searchOpen
                        property string actionLabel: qsTr("Search messages")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Search room messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleSearchPanel()
                    }
                    IconButton {
                        id: memberPanelButton
                        objectName: "memberPanelButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== "" && app.roomInfo.supported
                        iconName: "group"
                        active: root.infoOpen && infoPanel.section === "people"
                        property string actionLabel: qsTr("Members")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Room members")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleMemberPanel()
                    }
                    IconButton {
                        id: roomInfoButton
                        objectName: "roomInfoButton"
                        property bool folded:
                            roomHeaderActions.foldedActions
                                .indexOf(objectName) >= 0
                        visible: available && !folded
                        property bool available:
                            app.currentRoomId !== "" && app.roomInfo.supported
                        iconName: "info"
                        active: root.infoOpen && infoPanel.section !== "people"
                        property string actionLabel: qsTr("Room information")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Room information")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleRoomInfo()
                    }
                    IconButton {
                        id: roomHeaderOverflowButton
                        objectName: "roomHeaderOverflowButton"
                        visible: app.currentRoomId !== ""
                        iconName: "more_vert"
                        active: roomHeaderOverflowMenu.opened
                        Accessible.name: qsTr("More room actions")
                        ToolTip.text: qsTr("More room actions")
                        ToolTip.visible: hovered && !roomHeaderOverflowMenu.opened
                        ToolTip.delay: 500
                        onClicked: roomHeaderOverflowMenu.popup(
                            roomHeaderOverflowButton,
                            roomHeaderOverflowButton.width
                            - roomHeaderOverflowMenu.width,
                            roomHeaderOverflowButton.height + AppTheme.spacing4)
                    }
                    CallFormatMenu {
                        id: callFormatMenu
                        roomId: app.currentRoomId
                    }
                    AppMenu {
                        id: roomHeaderOverflowMenu
                        objectName: "roomHeaderOverflowMenu"
                        AppMenuItem {
                            objectName: "overflowStartVoiceCall"
                            visible: startVoiceCallButton.folded
                            iconName: "call"
                            text: startVoiceCallButton.actionLabel
                            onTriggered: startVoiceCallButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "overflowCallOptions"
                            visible: startVoiceCallButton.folded
                            iconName: "videocam"
                            text: qsTr("Call options")
                            onTriggered: Qt.callLater(function () {
                                callFormatMenu.popup(
                                    roomHeaderOverflowButton,
                                    roomHeaderOverflowButton.width
                                    - callFormatMenu.width,
                                    roomHeaderOverflowButton.height
                                    + AppTheme.spacing4)
                            })
                        }
                        AppMenuItem {
                            objectName: "overflowPinnedMessages"
                            visible: pinnedMessagesButton.folded
                            iconName: "push_pin"
                            text: pinnedMessagesButton.actionLabel
                            onTriggered: pinnedMessagesButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "overflowThreads"
                            visible: threadsViewButton.folded
                            iconName: "forum"
                            text: threadsViewButton.actionLabel
                            onTriggered: threadsViewButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "overflowSearch"
                            visible: timelineSearchButton.folded
                            iconName: "search"
                            text: timelineSearchButton.actionLabel
                            onTriggered: timelineSearchButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "overflowMembers"
                            visible: memberPanelButton.folded
                            iconName: "group"
                            text: memberPanelButton.actionLabel
                            onTriggered: memberPanelButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "overflowRoomInfo"
                            visible: roomInfoButton.folded
                            iconName: "info"
                            text: roomInfoButton.actionLabel
                            onTriggered: roomInfoButton.clicked()
                        }
                        AppMenuSeparator {
                            visible: roomHeaderActions.foldedActions.length > 0
                        }
                        AppMenuItem {
                            objectName: "overflowThreadReplies"
                            visible: threadRepliesToggleButton.folded
                            iconName: threadRepliesToggleButton.iconName
                            text: threadRepliesToggleButton.actionLabel
                            onTriggered: threadRepliesToggleButton.clicked()
                        }
                        AppMenuItem {
                            objectName: "roomHeaderChatBackground"
                            visible: !!app.backdrops
                            iconName: "image"
                            text: qsTr("Chat background…")
                            onTriggered: roomChatBackgroundDialog.openForRoom(
                                             app.currentRoomId,
                                             root.currentRoom.name || "")
                        }
                    }
                    ChatBackgroundDialog {
                        id: roomChatBackgroundDialog
                        objectName: "roomChatBackgroundDialog"
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.headerDivider }

        Rectangle {
            id: threadFilterBar
            objectName: "threadFilterBar"
            readonly property string rootId:
                app.timeline.threadFilterRootId || ""
            readonly property bool repliesHidden:
                app.timeline.hideThreadReplies === true
            visible: app.timeline.threadRepliesInline === true
                     && (rootId !== "" || repliesHidden)
            Layout.fillWidth: true
            implicitHeight: visible ? filterBarRow.implicitHeight
                                      + AppTheme.spacingS * 2 : 0
            color: AppTheme.timelineBackground
            RowLayout {
                id: filterBarRow
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing16
                anchors.rightMargin: AppTheme.spacing12
                spacing: AppTheme.spacingS
                Icon {
                    name: threadFilterBar.rootId !== "" ? "forum"
                                                        : "visibility_off"
                    size: 16
                    color: threadFilterBar.rootId !== ""
                           ? AppTheme.userColor(threadFilterBar.rootId)
                           : AppTheme.textMuted
                }
                Label {
                    objectName: "threadFilterLabel"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    text: {
                        if (threadFilterBar.rootId === "")
                            return qsTr("Thread replies are hidden")
                        var preview = app.timeline.replyPreviewForEvent(
                                    threadFilterBar.rootId) || ""
                        return preview.length > 0
                               ? qsTr("Thread: %1").arg(preview)
                               : qsTr("Showing one thread")
                    }
                }
                AppButton {
                    objectName: "threadFilterClearButton"
                    text: threadFilterBar.rootId !== ""
                          ? qsTr("Back to the room")
                          : qsTr("Show thread replies")
                    onClicked: {
                        if (threadFilterBar.rootId !== "")
                            app.timeline.clearThreadFilters()
                        else
                            app.settings.hideThreadReplies = false
                    }
                }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: AppTheme.headerDivider
            }
        }

        VoicePlayerBar {
            id: voiceDock
            objectName: "voicePlayerDock"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            shown: root.voiceDockShown
        }

        CallHeaderBar {
            id: timelineCallHeaderBar
            objectName: "timelineCallHeaderBar"
            Layout.fillWidth: true
            onParticipantsRequested: root.infoOpen = !root.infoOpen
        }

        Loader {
            id: legacyCallVideoHost
            objectName: "legacyCallVideoHost"
            Layout.fillWidth: true
            Layout.preferredHeight: active
                ? Math.min(Math.round(root.height * 0.45),
                           Math.round(width * 9 / 16))
                : 0
            active: app.calls.videoCall
                    && timelineCallHeaderBar.legacyLive
                    && app.calls.activeRoomId === app.currentRoomId
            visible: active
            sourceComponent: LegacyCallVideo {}
        }

        Loader {
            id: callStageHost
            objectName: "timelineCallStageHost"
            Layout.fillWidth: true
            Layout.preferredHeight: active ? root.callPanelHeight : 0
            active: root.callStageOwnsColumn
            visible: active
            sourceComponent: CallStage {
                objectName: "timelineCallStage"
                collapsed: root.callPanelCollapsed
                onParticipantsRequested: root.infoOpen = !root.infoOpen
                onCollapseToggled: root.callPanelCollapsed = !root.callPanelCollapsed
            }
        }

        Item {
            objectName: "callPanelDivider"
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 9 : 0
            visible: root.callStageOwnsColumn && !root.callPanelCollapsed

            Rectangle {
                anchors.centerIn: parent
                width: parent.width
                height: 1
                color: dividerHover.hovered || dividerDrag.active
                       ? AppTheme.accentBorder : AppTheme.border
            }

            HoverHandler {
                id: dividerHover
                cursorShape: Qt.SizeVerCursor
            }
            DragHandler {
                id: dividerDrag
                target: null
                yAxis.enabled: true
                xAxis.enabled: false
                cursorShape: Qt.SizeVerCursor
                onActiveChanged: {
                    if (dividerDrag.active) {
                        root.callPanelDragBase = root.callPanelHeight;
                    } else {
                        root.callPanelUserHeight =
                                root.clampCallPanelHeight(root.callPanelHeight);
                    }
                }
                onActiveTranslationChanged: {
                    if (!dividerDrag.active)
                        return;
                    root.callPanelUserHeight = root.clampCallPanelHeight(
                                root.callPanelDragBase
                                + dividerDrag.activeTranslation.y);
                }
            }
        }

        RoomCallBanner {
            objectName: "timelineRoomCallBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: visible ? AppTheme.spacing8 : 0
            roomId: app.currentRoomId
        }

        Rectangle {
            id: roomUpgradeBanner
            objectName: "roomUpgradeBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            readonly property bool showUpgraded: app.roomUpgrade.upgraded
            readonly property bool showPredecessor:
                app.roomUpgrade.predecessorRoomId.length > 0
            visible: showUpgraded || showPredecessor
            implicitHeight: upgradeCol.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusLg
            color: AppTheme.chipInfoFill
            border.color: AppTheme.chipInfoBorder
            border.width: 1

            ColumnLayout {
                id: upgradeCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacingS

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: roomUpgradeBanner.showUpgraded

                    Icon {
                        Layout.alignment: Qt.AlignVCenter
                        name: "info"
                        size: 18
                        color: AppTheme.chipInfoInk
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("This room has been upgraded.")
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        font.weight: AppTheme.weightMedium
                        font.family: AppTheme.uiFont
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
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

                Text {
                    objectName: "roomUpgradeError"
                    Layout.fillWidth: true
                    visible: app.roomUpgrade.error.length > 0
                    text: app.roomUpgrade.error
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.family: AppTheme.uiFont
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: roomUpgradeBanner.showPredecessor

                    Icon {
                        Layout.alignment: Qt.AlignVCenter
                        name: "arrow_back"
                        size: 18
                        color: AppTheme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("This room replaced an earlier one.")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.family: AppTheme.uiFont
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    AppButton {
                        objectName: "roomUpgradePreviousButton"
                        text: qsTr("Previous room")
                        kind: "secondary"
                        onClicked: app.roomUpgrade.goToPredecessor()
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open the previous room")
                    }
                }
            }
        }

        Rectangle {
            id: spaceParentBanner
            objectName: "spaceParentBanner"
            property string joiningSpaceId: ""
            property string joinError: ""
            readonly property bool joining: joiningSpaceId.length > 0
                                            && joiningSpaceId === spaceId
            Connections {
                target: app.discovery
                function onSpaceJoinedInPlace(joinedId) {
                    if (joinedId === spaceParentBanner.joiningSpaceId)
                        spaceParentBanner.joiningSpaceId = ""
                }
                function onJoinFailed(target, message) {
                    if (target !== spaceParentBanner.joiningSpaceId)
                        return
                    spaceParentBanner.joiningSpaceId = ""
                    spaceParentBanner.joinError = message
                }
            }
            onSpaceIdChanged: joinError = ""
            function followRoom() {
                if (app.currentRoomId !== "" && app.roomInfo.supported)
                    app.roomInfo.roomId = app.currentRoomId
            }
            Component.onCompleted: followRoom()
            Connections {
                target: app
                function onCurrentRoomIdChanged() { spaceParentBanner.followRoom() }
            }
            readonly property var parentSpace: app.roomInfo
                ? app.roomInfo.canonicalSpaceParent : ({})
            readonly property string spaceId:
                parentSpace && parentSpace.spaceId ? parentSpace.spaceId : ""
            readonly property string spaceName:
                parentSpace && parentSpace.name ? parentSpace.name : ""
            readonly property bool dismissedForSpace:
                spaceId.length > 0
                && app.settings.dismissedSpaceBanners.indexOf(spaceId) >= 0
            function dismiss() {
                app.settings.dismissSpaceBanner(spaceId)
            }
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: visible ? AppTheme.spacing8 : 0
            visible: app.roomInfo
                     && app.roomInfo.supported
                     && app.roomInfo.roomId === app.currentRoomId
                     && spaceId.length > 0
                     && parentSpace.joined !== true
                     && !dismissedForSpace
            implicitHeight: visible ? spaceParentRow.implicitHeight
                                      + AppTheme.spacing8 * 2 : 0
            radius: AppTheme.radiusLg
            color: AppTheme.chipInfoFill
            border.color: AppTheme.chipInfoBorder
            border.width: 1

            RowLayout {
                id: spaceParentRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacingS

                Icon {
                    objectName: "spaceParentInfoIcon"
                    Layout.alignment: Qt.AlignVCenter
                    name: "info"
                    size: 18
                    color: AppTheme.chipInfoInk
                    HoverHandler { id: spaceParentInfoHover }
                    ToolTip.text: qsTr("Joining only adds the Space to your sidebar and lets you see its rooms. It does not join you to any of its other rooms. Other members of the Space can see that you joined it. You can leave at any time, and you can also join later from this room's info panel.")
                    ToolTip.visible: spaceParentInfoHover.hovered
                    ToolTip.delay: 400
                    Accessible.role: Accessible.StaticText
                    Accessible.name: ToolTip.text
                }
                Text {
                    objectName: "spaceParentBannerText"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: spaceParentBanner.joinError.length > 0
                          ? qsTr("Could not join: %1").arg(spaceParentBanner.joinError)
                          : spaceParentBanner.spaceName.length > 0
                          ? qsTr("This room is in %1. Joining adds the Space to your sidebar; you can leave at any time.").arg(spaceParentBanner.spaceName)
                          : qsTr("This room is in a Space you have not joined. Joining adds the Space to your sidebar; you can leave at any time.")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: AppTheme.weightMedium
                    font.family: AppTheme.uiFont
                    elide: Text.ElideRight
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                AppButton {
                    objectName: "spaceParentJoinButton"
                    text: spaceParentBanner.joining ? qsTr("Joining…")
                          : qsTr("Join space")
                    kind: "primary"
                    enabled: !app.discovery.busy
                    onClicked: {
                        spaceParentBanner.joinError = ""
                        spaceParentBanner.joiningSpaceId = spaceParentBanner.spaceId
                        app.discovery.joinSpaceKeepingRoom(
                            spaceParentBanner.spaceId,
                            spaceParentBanner.parentSpace.via || [])
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Join the space this room belongs to")
                }
                AppButton {
                    objectName: "spaceParentDismissButton"
                    text: qsTr("Dismiss")
                    kind: "secondary"
                    onClicked: spaceParentBanner.dismiss()
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Dismiss this notice for this Space")
                }
            }
        }

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
            GridLayout {
                id: findRow
                objectName: "timelineFindRow"
                readonly property bool narrow: findBar.width < 520
                Layout.fillWidth: true
                columns: narrow ? 5 : 8
                columnSpacing: AppTheme.spacingS
                rowSpacing: AppTheme.spacingS
                SegmentedControl {
                    id: findModeToggle
                    objectName: "findModeToggle"
                    visible: root.findHistoryAvailable
                    dense: true
                    Layout.fillWidth: false
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
                            app.messageSearch.refreshIndexStats()
                            app.messageSearch.source =
                                app.messageSearch.localAvailable
                                ? "local" : "server"
                            app.timeline.endSearch()
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.filters = ({})
                            app.messageSearch.query = findField.text
                        } else {
                            app.messageSearch.query = ""
                            root.findScrolledEventId = ""
                            app.timeline.beginSearch(findField.text)
                        }
                        findField.forceActiveFocus()
                    }
                }
                IconButton {
                    objectName: "jumpToDateButton"
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "schedule"
                    iconSize: 18
                    enabled: app.currentRoomId !== ""
                    Accessible.name: qsTr("Jump to date")
                    ToolTip.text: qsTr("Jump to a date in this room")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: jumpToDateDialog.openDialog()
                }
                Item {
                    visible: findRow.narrow
                    Layout.fillWidth: true
                    Layout.columnSpan: findRow.columns - 1
                                       - (findModeToggle.visible ? 1 : 0)
                    implicitHeight: 1
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
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.query = text
                        } else {
                            root.findScrolledEventId = ""
                            app.timeline.updateSearch(text)
                        }
                    }
                    Keys.onReturnPressed: (event) => {
                        if (root.findHistoryMode) {
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.search()
                        } else {
                            root.stepFind(!(event.modifiers & Qt.ShiftModifier))
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
                          : (findField.text.trim().length > 0
                             ? qsTr("No matches") : "")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
                IconButton {
                    visible: !root.findHistoryMode
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "expand_less"
                    iconSize: 16
                    enabled: app.timeline.searchResultCount > 0
                    Accessible.name: qsTr("Previous match")
                    onClicked: root.stepFind(false)
                }
                IconButton {
                    visible: !root.findHistoryMode
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "expand_more"
                    iconSize: 16
                    enabled: app.timeline.searchResultCount > 0
                    Accessible.name: qsTr("Next match")
                    onClicked: root.stepFind(true)
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

            RowLayout {
                id: findNoMatchesRow
                objectName: "findNoMatchesRow"
                readonly property string keyword: findField.text.trim()
                visible: keyword.length > 0
                         && app.messageSearch.supported
                         && (root.findHistoryMode
                             ? app.messageSearch.state === "no_results"
                             : app.timeline.searchResultCount === 0)
                Layout.fillWidth: true
                spacing: AppTheme.spacingS
                Label {
                    objectName: "findNoMatchesLabel"
                    visible: !root.findHistoryMode
                    text: qsTr("No matches in loaded messages")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "findSearchAllRoomsButton"
                    kind: "ghost"
                    text: qsTr("Search all rooms for '%1'").arg(
                              findNoMatchesRow.keyword.length > 40
                              ? findNoMatchesRow.keyword.substring(0, 40) + "…"
                              : findNoMatchesRow.keyword)
                    onClicked: {
                        var q = findNoMatchesRow.keyword
                        root.closeFind()
                        root.searchAllRoomsRequested(q)
                    }
                }
            }

            RowLayout {
                objectName: "findLocalCoverageRow"
                Layout.fillWidth: true
                visible: root.findHistoryMode
                         && app.messageSearch.source === "local"
                spacing: AppTheme.spacingS
                Label {
                    objectName: "findLocalCoverage"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    text: {
                        if (app.messageSearch.indexing)
                            return qsTr("Indexing this room's history…")
                        if (app.messageSearch.state === "too_short")
                            return qsTr("Type at least %1 characters.")
                                .arg(app.messageSearch.minLocalChars)
                        var n = app.messageSearch.indexedMessages
                        if (n <= 0 && app.messageSearch.count > 0)
                            return ""
                        if (n <= 0)
                            return qsTr("Nothing is indexed yet.")
                        return qsTr("Searching %1 messages Lightning has "
                                    + "indexed, including encrypted ones.")
                                   .arg(n)
                    }
                }
                AppBusyIndicator {
                    visible: app.messageSearch.indexing
                    implicitWidth: 16
                    implicitHeight: 16
                }
                AppButton {
                    objectName: "findIndexRoomButton"
                    text: qsTr("Index this room")
                    kind: "ghost"
                    enabled: !app.messageSearch.indexing
                             && app.currentRoomId !== ""
                    ToolTip.text: qsTr("Fetch this room's older messages so "
                                       + "they can be searched")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: app.messageSearch.indexRoomHistory(
                                   app.currentRoomId)
                }
            }
            SegmentedControl {
                objectName: "findSourceToggle"
                dense: true
                Layout.fillWidth: false
                visible: root.findHistoryMode
                         && app.messageSearch.localAvailable
                         && root.serverSearchAvailable
                model: [
                    { label: qsTr("Indexed"), value: "local",
                      tip: qsTr("Lightning's own index. Works in encrypted "
                                + "rooms.") },
                    { label: qsTr("Server"), value: "server",
                      tip: qsTr("Your homeserver's search. Covers history "
                                + "this device has never seen, and cannot "
                                + "read encrypted rooms.") }
                ]
                current: app.messageSearch.source
                onActivated: (value) => app.messageSearch.source = value
            }

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
                ScrollBar.vertical: AppScrollBar {}
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
                            app.recordSearchResult(app.currentRoomId,
                                                   historyRow.eventId)
                            app.pagination.jumpToEvent(historyRow.eventId)
                        }
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Jump to message from %1").arg(
                        historyRow.senderDisplayName.length > 0
                            ? historyRow.senderDisplayName
                            : historyRow.sender)
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
                                textFormat: Text.PlainText
                                text: historyRow.senderDisplayName.length > 0
                                      ? historyRow.senderDisplayName
                                      : historyRow.sender
                                color: AppTheme.text
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var d = new Date(Number(
                                        historyRow.timestampMs))
                                    return d.toLocaleDateString(
                                        Qt.locale(AppLocale.name), Locale.ShortFormat)
                                }
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: historyRow.body
                            color: AppTheme.textSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                color: app.messageSearch.state === "error"
                       ? AppTheme.danger : AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                elide: Label.ElideRight
            }
            }
        }

        Item {
            id: timelineArea
            visible: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            ChatBackdrop {
                id: chatBackdrop
                anchors.fill: parent
                z: -1
                roomId: app.currentRoomId
                edgeTop: root.height > 0 ? timelineArea.y / root.height : 0
                edgeBottom: root.height > 0
                            ? (timelineArea.y + timelineArea.height) / root.height
                            : 1
            }

            Item {
                id: hoverResyncProbe
                objectName: "timelineHoverResyncProbe"
                visible: false
                width: 0
                height: 0
            }

            Flickable {
                id: timeline
                enabled: !root.invitePreview
                property int mediaBandFirstRow: 0
                property int mediaBandLastRow: 2147483647
                property int mediaRetainFirstRow: 0
                property int mediaRetainLastRow: 2147483647
                property bool mediaBandOpen: true
                function refreshMediaBand() {
                    if (count <= 0) {
                        mediaBandFirstRow = 0
                        mediaBandLastRow = 2147483647
                        mediaBandOpen = true
                        mediaRetainFirstRow = 0
                        mediaRetainLastRow = 2147483647
                        return
                    }
                    var h = Math.max(height, 400)
                    var far = contentY + h * 2.5
                    var last = viewRowAtContentY(far)
                    mediaBandFirstRow = 0
                    mediaBandOpen = far >= contentHeight
                    mediaBandLastRow = (mediaBandOpen || last < 0)
                                       ? count - 1 : last
                    var near = viewRowAtContentY(contentY - h * 2.5)
                    mediaRetainFirstRow = Math.max(0, near < 0 ? 0 : near)
                    mediaRetainLastRow = Math.max(mediaRetainFirstRow,
                        (far >= contentHeight || last < 0) ? count - 1 : last)
                }
                Timer {
                    id: mediaBandGrowth
                    interval: 100
                    repeat: false
                    onTriggered: {
                        if (!timeline.userScrollActive)
                            timeline.refreshMediaBand()
                    }
                }
                Timer {
                    id: mediaBandSettle
                    interval: 300
                    repeat: false
                    onTriggered: timeline.refreshMediaBand()
                }
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
                rotation: 180
                flickableDirection: Flickable.VerticalFlick
                opacity: presentationReady ? 1 : 0
                readonly property int count: rowRepeater.count
                contentWidth: width
                contentHeight: rowColumn.height

                property int visibleFirstRow: 0
                property int visibleLastRow: -1
                property int viewportNewestRow: 0
                property int viewportOldestRow: -1
                readonly property bool firstUnreadLoaded:
                    !!app.timeline && app.timeline.firstUnreadRow >= 0
                readonly property int firstUnreadViewRow:
                    firstUnreadLoaded
                    ? app.timeline.count - 1 - rowWindowSkip
                      - app.timeline.firstUnreadRow
                    : -1
                readonly property bool firstUnreadOnScreen:
                    firstUnreadLoaded && viewportOldestRow >= 0
                    && firstUnreadViewRow >= viewportNewestRow
                    && firstUnreadViewRow <= viewportOldestRow
                readonly property bool firstUnreadBelow:
                    firstUnreadLoaded && firstUnreadViewRow < viewportNewestRow
                property bool firstUnreadSeen: false
                property string firstUnreadSeenId: ""
                readonly property bool firstUnreadPassed:
                    firstUnreadSeen && firstUnreadLoaded
                    && firstUnreadSeenId === app.timeline.stableIdAt(
                           app.timeline.firstUnreadRow)
                function noteFirstUnreadSeen() {
                    if (!firstUnreadOnScreen || !presentationReady)
                        return
                    firstUnreadSeen = true
                    firstUnreadSeenId = app.timeline.stableIdAt(
                                app.timeline.firstUnreadRow)
                }
                onFirstUnreadOnScreenChanged: noteFirstUnreadSeen()
                onPresentationReadyChanged: noteFirstUnreadSeen()
                onFirstUnreadLoadedChanged: {
                    if (!firstUnreadLoaded) {
                        firstUnreadSeen = false
                        firstUnreadSeenId = ""
                    }
                }
                Timer {
                    id: visibleRowRangeTimer
                    interval: 0
                    onTriggered: timeline.updateVisibleRowRange()
                }
                function updateVisibleRowRange() {
                    if (count <= 0) {
                        visibleFirstRow = 0
                        visibleLastRow = -1
                        viewportNewestRow = 0
                        viewportOldestRow = -1
                        return
                    }
                    var first = viewRowAtContentY(contentY - height)
                    var last = viewRowAtContentY(contentY + height * 2)
                    visibleFirstRow = first < 0 ? 0 : first
                    visibleLastRow = last < 0 ? count - 1 : last
                    var newest = viewRowAtContentY(Math.max(0, contentY))
                    viewportNewestRow = newest < 0 ? 0 : newest
                    viewportOldestRow = viewRowAtPhysicalTop()
                }
                function scheduleVisibleRowRange() {
                    visibleRowRangeTimer.restart()
                }
                Timer {
                    id: presentationResetSettledTimer
                    interval: 0
                    onTriggered: {
                        timeline.presentationResetPending = false
                        timeline.recomputePresentationReady()
                        timeline.diagNavigationShape("reset-settled")
                    }
                }

                Column {
                    id: rowColumn
                    // A Flickable's left/right margins only widen the flick
                    // range, so inset the content directly. The rotation swaps
                    // left and right, harmless while equal.
                    x: timeline.leftMargin
                    width: {
                        var available = timeline.width - timeline.leftMargin
                                      - timeline.rightMargin
                        return available > 0 ? available : 640
                    }
                    spacing: 0

                    onPositioningComplete: {
                        timeline.layoutRowsAtLastPass = timeline.count
                        if (timeline.presentationGeometryStale) {
                            timeline.presentationGeometryStale = false
                            timeline.recomputePresentationReady()
                        }
                    }

                    Repeater {
                        id: rowRepeater
                        model: app.timelineView
                        // Rows are built synchronously as direct Column
                        // children. An asynchronous Loader made rows
                        // zero-height until incubated, which squeezed the
                        // column and was slower overall.
                        delegate: MessageDelegate {
                            width: rowColumn.width
                            rotation: 180
                            timelineView: timeline
                            rowOnScreen: index >= timeline.visibleFirstRow
                                         && index <= timeline.visibleLastRow
                            enabled: rowOnScreen
                            mediaInBand: index >= timeline.mediaBandFirstRow
                                         && index <= timeline.mediaBandLastRow
                            mediaPixmapRetained:
                                index >= timeline.mediaRetainFirstRow
                                && index <= timeline.mediaRetainLastRow
                        }
                    }
                }

                boundsBehavior: Flickable.StopAtBounds
                topMargin: AppTheme.spacingM
                bottomMargin: AppTheme.spacingM
                // The avatar gutter is rightMargin, not leftMargin: the 180°
                // rotation swaps sides, and at its left bound a Flickable
                // offsets content by +leftMargin on top of rowColumn's own x.
                // Visual left inset = rightMargin - leftMargin; visual right
                // inset = 2 * leftMargin.
                readonly property real avatarGutter: 20
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM + avatarGutter

                readonly property bool smoothScrollingEnabled: {
                    if (typeof app === "undefined" || !app || !app.settings)
                        return true
                    var v = app.settings.smoothScrolling
                    return v === undefined ? true : !!v
                }

                property bool stickToBottom: true

                readonly property real bottomFollowSlack: 8
                function atBottomEdge() {
                    if (rowWindowSkip > 0)
                        return false
                    return atYBeginning
                           || contentY <= wheelMinY() + bottomFollowSlack
                }
                readonly property int rowWindowSkip:
                    app.timelineView && app.timelineView.windowSkip !== undefined
                    ? app.timelineView.windowSkip : 0

                property bool presentationReady: app.currentRoomId === ""
                property bool presentationResetPending: false
                property bool presentationGeometryStale: false
                property bool presentationResetHadRows: false
                property bool restoreHoldsGate: false
                function releaseRestoreHold() {
                    if (!restoreHoldsGate)
                        return
                    restoreHoldsGate = false
                    restoreHoldGuard.stop()
                    if (!presentationReady)
                        presentationGuard.restart()
                    recomputePresentationReady()
                }
                Timer {
                    id: restoreHoldGuard
                    interval: 8000
                    onTriggered: timeline.releaseRestoreHold()
                }
                function recomputePresentationReady() {
                    if (presentationReady || presentationResetPending
                            || restoreHoldsGate)
                        return
                    if (app.currentRoomId === "") {
                        presentationReady = true
                        presentationGuard.stop()
                        return
                    }
                    var fillsViewport = count > 0 && height > 0
                                        && !presentationGeometryStale
                                        && contentHeight >= height - 1
                    if (fillsViewport || app.pagination.initialContentSettled) {
                        presentationReady = true
                        presentationGuard.stop()
                        if (stickToBottom)
                            scrollToEndTimer.restart()
                    }
                }
                Timer {
                    id: presentationGuard
                    interval: 2500
                    onTriggered: {
                        if (timeline.restoreHoldsGate)
                            return
                        if (!timeline.presentationReady) {
                            timeline.presentationReady = true
                            if (timeline.stickToBottom)
                                scrollToEndTimer.restart()
                        }
                    }
                }
                Connections {
                    target: app.pagination
                    function onStateChanged() {
                        timeline.recomputePresentationReady()
                        if (!app.pagination.busy)
                            timeline.maybeFillViewport()
                    }
                }

                property string viewAnchorId: ""
                property real viewAnchorOffset: 0
                property real viewAnchorLastY: 0
                property int viewAnchorCount: 0
                property int viewAnchorRow: -1
                property string viewAnchorKind: ""
                property real viewAnchorContentHeight: 0
                property real viewAnchorOriginY: 0
                function captureViewAnchor() {
                    if (newestEndInsertedRows > 0 && viewAnchorId !== ""
                            && !stickToBottom && count > 0
                            && userScrollActive && !moving && !scrollBarHeld) {
                        rowColumn.forceLayout()
                        maintainViewAnchor()
                    }
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
                    // Rows created since the last positioning pass still sit at y 0.
                    if (layoutRowsAtLastPass !== count)
                        rowColumn.forceLayout()
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
                    viewAnchorKind = it && it.rowKind !== undefined
                                     ? it.rowKind : ""
                    viewAnchorContentHeight = contentHeight
                    if (scrollTrace)
                        viewAnchorOriginY = originY
                }
                property int newestEndInsertedRows: 0
                Connections {
                    target: app.timelineView
                    function onRowsInserted(parent, first, last) {
                        if (first === 0)
                            timeline.newestEndInsertedRows += last - first + 1
                    }
                    function onModelReset() {
                        timeline.newestEndInsertedRows = 0
                    }
                }
                property bool glideTowardsNewer: false
                function insertedBelowAnchorHeight(delta, newestInserted) {
                    if (moving || scrollBarHeld || navigationPendingRow >= 0)
                        return 0
                    if (wheelAnimating
                            && (glideTowardsNewer || followLatestOnArrival))
                        return 0
                    if (delta <= 0.5 || newestInserted <= 0)
                        return 0
                    var sum = 0
                    for (var r = 0; r < newestInserted; ++r) {
                        var item = itemAtViewRow(r)
                        if (!item)
                            return 0
                        sum += Math.max(0, item.height)
                    }
                    return Math.abs(sum - delta) <= 1 ? delta : 0
                }
                property bool viewAnchorScheduled: false
                function maintainViewAnchorCoalesced() {
                    if (viewAnchorScheduled)
                        return
                    viewAnchorScheduled = true
                    Qt.callLater(function() {
                        if (!timeline) return
                        viewAnchorScheduled = false
                        timeline.maintainViewAnchor()
                    })
                }
                function maintainViewAnchor() {
                    var newestInserted = newestEndInsertedRows
                    newestEndInsertedRows = 0
                    if (viewAnchorId === "" || stickToBottom) {
                        if (scrollTrace) {
                            if (viewAnchorId === "")
                                diagNoAnchorReturns += 1
                            else
                                diagStickToBottomReturns += 1
                        }
                        return
                    }
                    var row = viewRowForStableId(viewAnchorId)
                    var it = row >= 0 ? itemAtViewRow(row) : null
                    var anchorY = anchorPositionForItem(it)
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
                        var grew = contentHeight - viewAnchorContentHeight
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
                            setPositionY(contentY + grew, "anchor-hold")
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
                        var deferredDelta = anchorY - viewAnchorLastY
                        var heldDelta = insertedBelowAnchorHeight(
                                    deferredDelta, newestInserted)
                        if (heldDelta > 0) {
                            if (scrollTrace) {
                                diagLiveInsertHolds += 1
                                diagLiveInsertHeldSum += heldDelta
                            }
                            setPositionY(Math.min(wheelMaxY(), contentY + heldDelta), "anchor-hold")
                            app.timelineScroll.translateActiveMotion(heldDelta)
                            captureViewAnchor()
                            return
                        }
                        diagNoteActiveDeferral(deferredDelta)
                        if (scrollTrace) {
                            diagMaterializedFirings += 1
                            if (Math.abs(deferredDelta)
                                    > Math.abs(diagMaterializedMaxAbsDelta))
                                diagMaterializedMaxAbsDelta = deferredDelta
                            if (!selfDrivenScrollActive)
                                diagDragDeferrals += 1
                        }
                        captureViewAnchor()
                        return
                    }
                    var desired = anchorY + viewAnchorOffset
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    desired = desired < lo ? lo : (desired > hi ? hi : desired)
                    if (Math.abs(contentY - desired) > 0.5) {
                        if (scrollTrace)
                            diagAnchorCorrections += 1
                        setPositionY(desired, "anchor-restore")
                    }
                    viewAnchorLastY = anchorY
                    viewAnchorCount = count
                    viewAnchorRow = row
                    viewAnchorContentHeight = contentHeight
                    if (scrollTrace)
                        viewAnchorOriginY = originY
                }

                property var timelineModel: app.timeline
                readonly property string keyboardSelectedEventId:
                    activeFocus || root.restoredSelectionOutline
                    ? root.keyboardSelectedEventId : ""

                property bool selectionDetached: false

                function messageIntersectsViewport(row) {
                    var item = row >= 0 ? itemAtViewRow(row) : null
                    return item && navigableEventAt(row) !== ""
                        && item.y < contentY + height
                        && item.y + item.height > contentY
                }

                function selectVisibleMessage(direction) {
                    var first = Math.max(0, viewRowAtContentY(contentY))
                    var candidate = ""
                    for (var row = first; row < count; ++row) {
                        var item = itemAtViewRow(row)
                        if (!item || item.y >= contentY + height)
                            break
                        if (messageIntersectsViewport(row)) {
                            candidate = navigableEventAt(row)
                            if (direction > 0)
                                break
                        }
                    }
                    root.keyboardSelectedEventId = candidate
                    selectionDetached = false
                    return candidate !== ""
                }

                function navigableEventAt(row) {
                    var item = itemAtViewRow(row)
                    if (!item || item.height <= 0 || !item.replyEligible)
                        return ""
                    return eventIdAtViewRow(row)
                }

                function selectNewestVisibleMessage() {
                    if (!selectVisibleMessage(1))
                        return false
                    ensureMessageVisible(viewRowForStableId(root.keyboardSelectedEventId))
                    return true
                }

                property bool enteringMessageNavigation: false
                Timer {
                    id: messageNavigationEntry
                    interval: 0
                    repeat: false
                    onTriggered: {
                        if (!timeline.enteringMessageNavigation
                                || !timeline.activeFocus || !root.timelineFocusable)
                            return
                        timeline.enteringMessageNavigation = false
                        timeline.positionViewAtLatest()
                        timeline.selectNewestVisibleMessage()
                    }
                }

                function enterMessageNavigation() {
                    selectionDetached = false
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    noteReaderTookControl()
                    settleAtLatest(true)
                    selectNewestVisibleMessage()
                    enteringMessageNavigation = true
                    messageNavigationEntry.restart()
                }

                function stopEntrySelection() {
                    enteringMessageNavigation = false
                    messageNavigationEntry.stop()
                }

                function moveKeyboardViewport(target) {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    noteReaderTookControl()
                    if (target < contentY && nearWindowNewEdge()) {
                        var beforeExtension = contentY
                        extendRowWindowAtNewEnd()
                        target += contentY - beforeExtension
                    }
                    setPositionY(Math.max(wheelMinY(), Math.min(wheelMaxY(), target)), "keyboard")
                    updateStickAndPaginate()
                    captureViewAnchor()
                    scrollSettleTimer.restart()
                }

                function ensureMessageVisible(row) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return
                    var target = contentY
                    if (item.height >= height || item.y < contentY)
                        target = item.y
                    else if (item.y + item.height > contentY + height)
                        target = item.y + item.height - height
                    if (target > contentY + 0.5 && item.height < height) {
                        var first = Math.max(0, viewRowAtContentY(target))
                        for (var candidate = first; candidate <= row; ++candidate) {
                            var edge = itemAtViewRow(candidate)
                            if (edge && edge.y >= target - 0.5
                                    && navigableEventAt(candidate) !== "") {
                                target = edge.y
                                break
                            }
                        }
                    }
                    if (Math.abs(target - contentY) > 0.5)
                        moveKeyboardViewport(target)
                }

                function stepSelectedMessage(direction) {
                    stopEntrySelection()
                    if (direction < 0 && nearWindowNewEdge())
                        extendRowWindowAtNewEnd()
                    var row = viewRowForStableId(root.keyboardSelectedEventId)
                    if (selectionDetached || !messageIntersectsViewport(row)) {
                        if (selectVisibleMessage(direction))
                            return true
                        row = viewRowAtContentY(contentY) + (direction < 0 ? 1 : 0)
                    }
                    for (var next = row + direction;
                         next >= 0 && next < count; next += direction) {
                        var eventId = navigableEventAt(next)
                        if (eventId !== "") {
                            root.keyboardSelectedEventId = eventId
                            ensureMessageVisible(next)
                            return true
                        }
                    }
                    if (direction > 0 && app.timeline.canPaginate)
                        app.pagination.requestNearTop(true)
                    return false
                }

                function navigationAction(key, modifiers) {
                    return activeFocus && root.timelineFocusable
                        ? app.shortcuts.messageSelectionActionForKey(key, modifiers)
                        : ""
                }

                function pageTimeline(direction) {
                    stopEntrySelection()
                    var before = contentY
                    moveKeyboardViewport(contentY - direction * height * 0.5)
                    if (Math.abs(contentY - before) > 0.5)
                        selectionDetached = true
                }

                function runNavigationAction(action) {
                    switch (action) {
                    case "timeline.olderMessage": stepSelectedMessage(1); break
                    case "timeline.newerMessage": stepSelectedMessage(-1); break
                    case "timeline.halfPageUp":
                        pageTimeline(-1)
                        break
                    case "timeline.halfPageDown":
                        pageTimeline(1)
                        break
                    case "timeline.reply":
                        stopEntrySelection()
                        var row = viewRowForStableId(root.keyboardSelectedEventId)
                        if (row >= 0 && navigableEventAt(row) !== ""
                                && root.composerFocusable) {
                            beginReplyForEvent(root.keyboardSelectedEventId)
                            root.focusComposerForTyping()
                        }
                        break
                    case "timeline.forwardMessage":
                    case "timeline.replyInThread":
                    case "timeline.editMessage":
                    case "timeline.reactToMessage":
                    case "timeline.pinMessage":
                    case "timeline.saveMessage":
                    case "timeline.deleteMessage":
                    case "timeline.copyMessageText":
                    case "timeline.copyMessageLink":
                    case "timeline.viewMessageSource":
                        return runSelectedMessageAction(action)
                    default: return false
                    }
                    return true
                }
                function runSelectedMessageAction(action) {
                    stopEntrySelection()
                    var row = viewRowForStableId(root.keyboardSelectedEventId)
                    if (row < 0 || navigableEventAt(row) === "")
                        return false
                    var item = itemAtViewRow(row)
                    return !!item && !!item.runSelectionAction
                           && item.runSelectionAction(action)
                }
                function releasePendingRows() {
                    if (!app.timelineView)
                        return
                    if (app.timelineView.clearWindow)
                        app.timelineView.clearWindow()
                    else if (app.timelineView.releaseAll)
                        app.timelineView.releaseAll()
                }
                function sourceRowForViewRowAtCount(row, rowCount) {
                    return row < 0 || row >= rowCount
                            ? -1
                            : app.timeline.count - 1 - rowWindowSkip - row
                }
                function sourceRowForViewRow(row) {
                    return sourceRowForViewRowAtCount(row, count)
                }
                function viewRowForSourceRow(row) {
                    var viewRow = app.timeline.count - 1 - rowWindowSkip - row
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
                function itemAtViewRow(row) {
                    return row < 0 || row >= count
                            ? null : rowRepeater.itemAt(row)
                }
                function rowSkippedByLayout(item) {
                    return !item || item.height <= 0 || item.width <= 0
                           || (rowColumn.visible && !item.visible)
                }
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
                        var probe = mid
                        while (probe >= lo && rowSkippedByLayout(item)) {
                            --probe
                            item = probe >= lo ? rowRepeater.itemAt(probe)
                                               : null
                        }
                        if (probe < lo) {
                            lo = mid + 1
                        } else if (item.y > y) {
                            hi = probe - 1
                        } else {
                            best = probe
                            lo = mid + 1
                        }
                    }
                    return best
                }
                function viewRowAtPhysicalTop() {
                    return viewRowAtContentY(
                        contentY + Math.max(0, height - topMargin - 1))
                }
                function anchorPositionForItem(item) {
                    return item ? item.y + item.height : 0
                }
                function positionViewAtLatest() {
                    setPositionY(wheelMinY(), "follow-latest")
                }
                readonly property real navigationInsetFraction: 0.33
                function positionViewAtNavigationTarget(row) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return false
                    var target = anchorPositionForItem(item) - height
                                 + topMargin + height * navigationInsetFraction
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    setPositionY(target < lo ? lo : (target > hi ? hi : target), "navigation")
                    return true
                }
                property int diagWindowUnmeasuredRows: 0
                property int diagNavigationLandings: 0
                property int diagNavigationUnresolved: 0
                property int diagNavigationAbandoned: 0
                function diagNavigationShape(tag) {
                    if (!scrollTrace || !app.timeline)
                        return
                    const total = app.timeline.count
                    const seen = ({})
                    let dupIds = 0
                    for (let r = 0; r < total; ++r) {
                        const id = app.timeline.eventIdAt(r)
                        if (id === "")
                            continue
                        if (seen[id] === true)
                            ++dupIds
                        else
                            seen[id] = true
                    }
                    let yInversions = 0
                    let prevBottom = -1
                    for (let v = 0; v < count; ++v) {
                        const it = rowRepeater.itemAt(v)
                        if (!it || it.height <= 0)
                            continue
                        if (prevBottom >= 0 && it.y + 0.5 < prevBottom)
                            ++yInversions
                        prevBottom = it.y + it.height
                    }
                    console.info("timeline-navigation-shape"
                                 + " tag=" + tag
                                 + " srcRows=" + total
                                 + " rows=" + count
                                 + " winSkip=" + rowWindowSkip
                                 + " laidOutRows=" + layoutRowsAtLastPass
                                 + " dupIds=" + dupIds
                                 + " yInversions=" + yInversions
                                 + " stick=" + (stickToBottom ? 1 : 0)
                                 + " firstUnreadSrc="
                                 + app.timeline.firstUnreadRow)
                }
                property int navigationPendingRow: -1
                property string navigationPendingId: ""
                property real navigationPendingOffset: 0
                property bool navigationPendingHighlight: false
                property int navigationPendingAttempts: 0
                property int navigationTotalAttempts: 0
                property string navigationLastShape: ""
                property int layoutRowsAtLastPass: 0
                function navigationGeometryReady(item) {
                    if (!item)
                        return false
                    if (item.height <= 0 && item.visible)
                        return false
                    return layoutRowsAtLastPass === count
                }
                readonly property int maxNavigationLandingAttempts: 12
                readonly property int maxNavigationLandingTicks: 120
                Timer {
                    id: navigationLandingTimer
                    interval: 16
                    repeat: false
                    onTriggered: timeline.tryLandNavigationTarget()
                }
                Timer {
                    id: firstUnreadRetryTimer
                    interval: 240
                    repeat: false
                    onTriggered: timeline.landOnFirstUnread()
                }
                function abandonNavigationLanding() {
                    if (navigationPendingRow < 0 && navigationPendingId === "")
                        return
                    navigationPendingRow = -1
                    navigationPendingId = ""
                    navigationPendingAttempts = 0
                    navigationTotalAttempts = 0
                    navigationLastShape = ""
                    navigationLandingTimer.stop()
                    ++diagNavigationAbandoned
                }
                property bool readerControlledSinceReset: false
                property int navigationEpoch: 0
                property int resetEpoch: 0
                function noteReaderTookControl() {
                    readerControlledSinceReset = true
                    ++navigationEpoch
                    releaseRestoreHold()
                    if (app.pagination)
                        app.pagination.cancelNavigation()
                    abandonNavigationLanding()
                    firstUnreadPagesLeft = 0
                    firstUnreadRetryTimer.stop()
                }
                function nestedWheelTaken() {
                    root.stopAutoscroll()
                    timeline.noteReaderTookControl()
                }
                function beginNavigationLanding(row, pixelOffset, highlight,
                                                knownStableId) {
                    navigationPendingId = knownStableId
                                          ? knownStableId
                                          : (app.timeline
                                             ? app.timeline.stableIdAt(row)
                                             : "")
                    navigationPendingRow = row
                    navigationPendingOffset = pixelOffset
                    navigationPendingHighlight = highlight
                    navigationPendingAttempts = 0
                    navigationTotalAttempts = 0
                    navigationLastShape = ""
                    tryLandNavigationTarget()
                }
                function tryLandNavigationTarget() {
                    if (navigationPendingRow < 0)
                        return false
                    const row = navigationPendingId !== ""
                                && app.timeline
                                ? app.timeline.rowForStableId(
                                      navigationPendingId)
                                : navigationPendingRow
                    const viewRow = row >= 0 ? viewRowForSourceRow(row) : -1
                    let item = viewRow >= 0 ? itemAtViewRow(viewRow) : null
                    if (item && !navigationGeometryReady(item)
                            && (item.height > 0 || !item.visible)) {
                        rowColumn.forceLayout()
                        item = itemAtViewRow(viewRow)
                    }
                    if (!navigationGeometryReady(item)) {
                        const shape = count + "/" + layoutRowsAtLastPass
                        if (shape !== navigationLastShape) {
                            navigationLastShape = shape
                            navigationPendingAttempts = 0
                        }
                        if (++navigationPendingAttempts
                                    < maxNavigationLandingAttempts
                                && ++navigationTotalAttempts
                                    < maxNavigationLandingTicks) {
                            navigationLandingTimer.restart()
                            return false
                        }
                        navigationPendingRow = -1
                        navigationPendingId = ""
                        releaseRestoreHold()
                        ++diagNavigationUnresolved
                        console.warn("timeline navigation target unresolved"
                                     + " sourceRow=" + row
                                     + " viewRow=" + viewRow
                                     + " rows=" + count
                                     + " srcRows="
                                     + (app.timeline ? app.timeline.count : -1)
                                     + " winSkip=" + rowWindowSkip
                                     + " built=" + (item ? 1 : 0)
                                     + " rowH=" + (item ? item.height : -1)
                                     + " laidOutRows=" + layoutRowsAtLastPass
                                     + " ticks=" + navigationTotalAttempts)
                        return false
                    }
                    rowColumn.forceLayout()
                    const pixelOffset = navigationPendingOffset
                    const highlight = navigationPendingHighlight
                    navigationPendingRow = -1
                    navigationPendingId = ""
                    cancelWheelMotion()
                    if (highlight) {
                        positionViewAtNavigationTarget(viewRow)
                    } else if (isNaN(pixelOffset)) {
                        positionViewAtViewRow(viewRow, true)
                    } else {
                        positionViewAtViewRow(viewRow, false)
                        setPositionY(anchorPositionForItem(item) + pixelOffset, "saved-restore")
                    }
                    stickToBottom = atBottomEdge()
                    ++diagNavigationLandings
                    releaseRestoreHold()
                    saveRoomPosition()
                    newestEndInsertedRows = 0
                    captureViewAnchor()
                    diagNavigationShape("landed")
                    return true
                }
                function positionViewAtViewRow(row, centered) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return
                    var target = centered
                            ? anchorPositionForItem(item)
                              - (height + item.height) / 2
                            : anchorPositionForItem(item) - height + topMargin
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    setPositionY(target < lo ? lo : (target > hi ? hi : target), "navigation")
                }
                property string suppressRootEventId: ""
                property bool threadContext: false

                property string pinnedActionsKey: ""
                property string hoveredActionsKey: ""
                readonly property bool rowHoverSuspended:
                    viewportMotionActive || middleClickScroller.active
                onRowHoverSuspendedChanged: {
                    if (!rowHoverSuspended)
                        resumeRowHover()
                }
                function resumeRowHover() {
                    hoverResyncProbe.x = hoverResyncProbe.x === 0 ? 1 : 0
                }
                property bool emojiPickerOpen: false
                property string transientInteractionOwner: ""
                function claimTransientInteraction(owner) {
                    if (!owner || owner.length === 0)
                        return
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
                property bool roomEncrypted: root.currentRoom.encrypted === true
                property bool isDirectRoom: root.currentRoom.isDirect === true
                property string roomJoinRule: root.currentRoom.joinRule || ""
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

                property var openImage: function(mediaKey, httpUrl) {
                    imageViewer.openFor(mediaKey || "", httpUrl)
                }
                property var openArtwork: function(source, title) {
                    imageViewer.openArtwork(source, title)
                }
                property var openReactionPicker: function(eventId, point, expanded) {
                    if (!eventId || eventId.length === 0)
                        return
                    sharedReactionPicker.targetEventId = eventId
                    sharedReactionPicker.anchorPoint = point
                    sharedReactionPicker.openExpanded = expanded === true
                    sharedReactionPicker.open()
                }
                property var closeTransientRowSurfaces: function() {
                    root.closeRowAnchoredSurfaces()
                }
                property var openSenderProfile: function(member, anchor) {
                    senderProfilePopover.openFor(member, anchor)
                }
                property var navigateToEvent: function(eventId) {
                    if (!eventId || eventId.length === 0)
                        return
                    root.stopAutoscroll()
                    timeline.cancelWheelMotion()
                    app.pagination.jumpToEvent(eventId)
                }
                readonly property string navigationHighlightEventId:
                    app.pagination.highlightedEventId
                property var openReceiptList: function(readers, totalOthers,
                                                       point, eventId) {
                    var list = readers || []
                    var total = totalOthers
                    if (eventId && timelineModel
                            && timelineModel.cumulativeReadersForEvent) {
                        var c = timelineModel.cumulativeReadersForEvent(eventId)
                        if (c && c.readers && c.readers.length >= list.length) {
                            list = c.readers
                            total = Math.max(c.total, c.readers.length)
                        }
                    }
                    receiptListPopover.readers = list
                    receiptListPopover.totalOthers = total
                    receiptListPopover.anchorPoint = point
                    receiptListPopover.open()
                }

                property var beginReplyForEvent: function(eventId) {
                    if (!eventId || eventId.length === 0) return
                    var details = timelineModel.messageDetails(eventId)
                    if (!details.eventId) return
                    var previewText = timelineModel.replyPreviewForEvent(eventId)
                    app.composer.beginReply(eventId,
                        details.senderName || details.senderId,
                        (previewText || "").substring(0, 80),
                        timelineModel.mediaKeyForEvent(eventId),
                        timelineModel.threadRepliesInline === true
                            ? (details.threadRootId || "") : "")
                }
                property var openThreadForEvent: function(eventId) {
                    if (!eventId || eventId.length === 0) return
                    var details = timelineModel.messageDetails(eventId)
                    var rootId = (details.threadRootId || "").length > 0
                                 ? details.threadRootId : eventId
                    app.showThread(app.currentRoomId, rootId)
                }
                property string downloadSurface: "main"
                property var saveMedia: function(mediaKey, filename, mime, ask) {
                    root.startDownload(mediaKey, filename, mime, ask, "main")
                }
                property string saveInFlightKey: ""
                property string lastSavedKey: ""
                property bool lastSaveOk: true
                property bool lastSaveCancelled: false
                property string cancelledKey: ""
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
                    if (!mediaKey || mediaKey === "")
                        return
                    if (saveInFlightKey === mediaKey)
                        saveInFlightKey = ""
                    lastSavedKey = mediaKey
                    lastSaveOk = ok
                    lastSaveCancelled = cancelledKey === mediaKey
                    cancelledKey = ""
                    savedFlashTimer.restart()
                }

                // Deferred via Qt.callLater: positioning synchronously in
                // onCountChanged during a reset made DelegateModel cancel an
                // out-of-range index. callLater coalesces requests and
                // re-checks state at fire time.
                // A child Timer rather than Qt.callLater: a queued callLater
                // can fire after the pane is gone (sign-out, room switch) and
                // throw "positionViewAtLatest is not a function". restart()
                // coalesces requests exactly as callLater did.
                Timer {
                    id: scrollToEndTimer
                    interval: 0
                    onTriggered: timeline.scrollToEndDeferred()
                }
                function scrollToEndDeferred() {
                    if (count > 0 && stickToBottom && !wheelAnimating)
                        positionViewAtLatest()
                }

                property bool wheelAnimating: app.timelineScroll.motionActive

                readonly property bool scrollBarHeld: timelineScrollBar.pressed

                readonly property bool userScrollActive:
                    moving || scrollBarHeld || wheelAnimating
                    || scrollSettleTimer.running

                readonly property bool speculativeMediaAllowed:
                    !userScrollActive

                readonly property bool viewportMotionActive:
                    moving || scrollBarHeld || wheelAnimating

                readonly property bool selfDrivenScrollActive:
                    !moving && !scrollBarHeld
                    && (wheelAnimating || scrollSettleTimer.running)

                readonly property bool scrollTrace: app.timelineScroll.scrollTraceEnabled
                property int perfMediaCached: 0
                property int perfPreviewRefreshes: 0
                function notePreviewRefresh() {
                    if (scrollTrace) perfPreviewRefreshes += 1
                }
                Connections {
                    target: app.mediaBridge
                    enabled: timeline.scrollTrace
                    function onMediaCached(cacheKey) {
                        timeline.perfMediaCached += 1
                    }
                }
                Timer {
                    interval: 10000
                    repeat: true
                    running: timeline.scrollTrace
                    onTriggered: {
                        console.info("scroll-perf-window"
                            + " rows=" + timeline.count
                            + " srcRows=" + (app.timeline ? app.timeline.count : -1)
                            + " winSkip=" + timeline.rowWindowSkip
                            + " visibleRows=" + Math.max(0, timeline.visibleLastRow
                                                       - timeline.visibleFirstRow + 1)
                            + " bandLast=" + timeline.mediaBandLastRow
                            + " mediaCached=" + timeline.perfMediaCached
                            + " previewRefreshes=" + timeline.perfPreviewRefreshes
                            + " motion=" + (timeline.userScrollActive ? 1 : 0))
                        timeline.perfMediaCached = 0
                        timeline.perfPreviewRefreshes = 0
                    }
                }
                property bool diagActive: false
                property int diagEvents: 0
                property int diagPixelEvents: 0
                property int diagAngleEvents: 0
                property int diagAnchorCorrections: 0
                property int diagGrowthCorrections: 0
                property int diagRowCount: 0
                property int diagNoAnchorReturns: 0
                property int diagStickToBottomReturns: 0
                property int diagDisplacedFirings: 0
                property real diagDisplacedAppliedSum: 0
                property real diagDisplacedMaxAbsGrew: 0
                property int diagDisplacedMaxAbsGrewRows: 0
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
                property string diagInputs: ""
                property int diagAutoscrollSteps: 0
                property int diagScrollbarMoves: 0
                property int diagScrollbarTopHolds: 0
                property int diagTopRequests: 0
                property int diagTopBudgetBlocked: 0
                property int diagLiveInsertHolds: 0
                property real diagLiveInsertHeldSum: 0
                function diagNoteInput(kind) {
                    if (!scrollTrace)
                        return
                    diagBegin()
                    if (diagInputs.split(",").indexOf(kind) < 0)
                        diagInputs = diagInputs === "" ? kind
                                                       : diagInputs + "," + kind
                    if (kind === "autoscroll")
                        diagAutoscrollSteps += 1
                    else if (kind === "scrollbar")
                        diagScrollbarMoves += 1
                }
                function diagNoteEvent(isPixel) {
                    if (!scrollTrace)
                        return
                    diagNoteInput("wheel")
                    diagEvents += 1
                    if (isPixel)
                        diagPixelEvents += 1
                    else
                        diagAngleEvents += 1
                }
                function diagBegin() {
                    if (!diagActive) {
                        diagActive = true
                        diagInputs = ""
                        diagAutoscrollSteps = 0
                        diagScrollbarMoves = 0
                        diagScrollbarTopHolds = 0
                        diagTopRequests = 0
                        diagTopBudgetBlocked = 0
                        diagEvents = 0
                        diagPixelEvents = 0
                        diagAngleEvents = 0
                        diagStartY = contentY
                        diagStartHeight = contentHeight
                        diagStartOriginY = originY
                        diagGestureStartMs = Date.now()
                        diagWorstNotchMs = 0
                        diagWindowApplications = 0
                        diagWindowNewEndExtensions = 0
                    }
                }
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
                property int diagWindowApplications: 0
                property int diagWindowNewEndExtensions: 0
                function diagFlushGesture() {
                    if (!scrollTrace || !diagActive)
                        return
                    diagActive = false
                    console.info("scroll-gesture"
                        + " inputs=" + (diagInputs === "" ? "none" : diagInputs)
                        + " autoSteps=" + diagAutoscrollSteps
                        + " barMoves=" + diagScrollbarMoves
                        + " barTopHolds=" + diagScrollbarTopHolds
                        + " topReqs=" + diagTopRequests
                        + " topBudgetBlocked=" + diagTopBudgetBlocked
                        + " approachRows=" + nearTopRowsThisApproach
                        + " approachReqs=" + nearTopRequestsThisApproach
                        + " armed=" + (nearTopArmed ? 1 : 0)
                        + " reachedStart=" + (app.pagination && app.pagination.reachedStart ? 1 : 0)
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
                        + " liveInsertHolds=" + diagLiveInsertHolds
                        + " liveInsertHeld=" + Math.round(diagLiveInsertHeldSum)
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
                        + " srcRows=" + (app.timeline ? app.timeline.count : -1)
                        + " winSkip=" + rowWindowSkip
                        + " winApplies=" + diagWindowApplications
                        + " winExtendNew=" + diagWindowNewEndExtensions
                        + " winUnmeasured=" + diagWindowUnmeasuredRows
                        + " gestureMs=" + Math.round(Date.now() - diagGestureStartMs)
                        + " worstNotchMs=" + Math.round(diagWorstNotchMs)
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
                    diagLiveInsertHolds = 0
                    diagLiveInsertHeldSum = 0
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

                function wheelMinY() { return originY - topMargin }
                function wheelMaxY() {
                    var maxY = originY + contentHeight + bottomMargin - height
                    var minY = wheelMinY()
                    return maxY < minY ? minY : maxY
                }
                readonly property int windowRunwayRows: 220
                readonly property int windowMarginRows: 120
                readonly property int windowMinRows: 320
                property int pendingWindowSkip: -1
                property int pendingWindowOldest: -1
                property int pendingWindowTotal: -1
                readonly property int windowTrimStepRows: 8
                Timer {
                    id: rowWindowTrimTimer
                    interval: 16
                    onTriggered: timeline.stepRowWindowTrim()
                }
                function applyRowWindow() {
                    if (!app.timelineView || !app.timelineView.setWindow)
                        return
                    if (viewportMotionActive || !presentationReady)
                        return
                    if (app.pagination && app.pagination.busy)
                        return
                    const total = app.timeline ? app.timeline.count : 0
                    if (total <= windowMinRows) {
                        if (rowWindowSkip > 0)
                            app.timelineView.clearWindow()
                        return
                    }
                    const skip = rowWindowSkip
                    const absNewestVisible = skip + Math.max(0, visibleFirstRow)
                    const absOldestVisible =
                        skip + Math.max(visibleLastRow, visibleFirstRow)
                    let wantSkip =
                        Math.max(0, absNewestVisible - windowRunwayRows)
                    let wantRows = (absOldestVisible + windowMarginRows)
                                   - wantSkip + 1
                    wantRows = Math.max(windowMinRows, wantRows)
                    if (wantSkip + wantRows > total)
                        wantRows = total - wantSkip
                    const rows = count
                    if (wantSkip !== 0
                        && Math.abs(wantSkip - skip) < 40
                        && Math.abs(wantRows - rows) < 40)
                        return
                    const tailFirst = (wantSkip - skip) + wantRows
                    if (tailFirst < rows) {
                        let tailRelease = 0
                        for (let t = tailFirst; t < rows; ++t) {
                            const tailItem = itemAtViewRow(t)
                            if (tailItem)
                                tailRelease += tailItem.height
                        }
                        if (distanceFromTop() - tailRelease
                            <= nearTopEnterDistance)
                            wantRows = rows - (wantSkip - skip)
                    }
                    if (wantSkip === skip && wantRows === rows)
                        return
                    pendingWindowSkip = wantSkip
                    pendingWindowOldest = wantSkip + wantRows
                    pendingWindowTotal = total
                    stepRowWindowTrim()
                }
                function stepRowWindowTrim() {
                    if (pendingWindowSkip < 0 || !app.timelineView
                        || viewportMotionActive
                        || !presentationReady || !app.timeline
                        || app.timeline.count !== pendingWindowTotal) {
                        pendingWindowSkip = -1
                        rowWindowTrimTimer.stop()
                        return
                    }
                    const skip = rowWindowSkip
                    const oldest = skip + count
                    const nextSkip = skip + Math.max(-windowTrimStepRows,
                        Math.min(windowTrimStepRows, pendingWindowSkip - skip))
                    const nextOldest = oldest + Math.max(-windowTrimStepRows,
                        Math.min(windowTrimStepRows, pendingWindowOldest - oldest))
                    const wantRows = nextOldest - nextSkip
                    const wantSkip = nextSkip
                    if (wantSkip === skip && wantRows === count) {
                        pendingWindowSkip = -1
                        return
                    }
                    const anchorRow = Math.max(0, viewRowAtContentY(contentY))
                    const anchorItem = itemAtViewRow(anchorRow)
                    const anchorEventId = eventIdAtViewRow(anchorRow)
                    if (!anchorItem || anchorEventId === "")
                    {
                        pendingWindowSkip = -1
                        return
                    }
                    const anchorOffset = anchorItem.y - contentY

                    const writerBeforeTrim = positionWriter
                    positionWriter = "window-trim"
                    app.timelineView.setWindow(wantSkip, wantRows)
                    rowColumn.forceLayout()
                    positionWriter = writerBeforeTrim

                    const movedRow = viewRowForStableId(anchorEventId)
                    const movedItem = movedRow >= 0 ? itemAtViewRow(movedRow)
                                                    : null
                    if (movedItem) {
                        setPositionY(movedItem.y - anchorOffset, "window-trim")
                    } else {
                        ++diagWindowUnmeasuredRows
                        pendingWindowSkip = -1
                        return
                    }
                    ++diagWindowApplications
                    newestEndInsertedRows = 0
                    updateStickAndPaginate()
                    captureViewAnchor()
                    if (wantSkip !== pendingWindowSkip
                        || nextOldest !== pendingWindowOldest)
                        rowWindowTrimTimer.start()
                    else
                        pendingWindowSkip = -1
                }

                function updateStickAndPaginate(followStateApplies) {
                    if (followStateApplies !== false)
                        stickToBottom = atBottomEdge()
                    checkNearTopEdge(true)
                }
                function wheelCanMove(towardsOlder) {
                    return towardsOlder ? contentY < wheelMaxY() - 0.5
                                        : contentY > wheelMinY() + 0.5
                }

                property bool nearTopCheckScheduled: false
                property bool nearTopCheckUserInitiated: false
                readonly property int windowNewEndExtendRows: 3
                Timer {
                    id: rowWindowExtendTimer
                    interval: 16
                    onTriggered: {
                        if (timeline.rowWindowSkip > 0
                            && timeline.contentY - timeline.wheelMinY()
                               < timeline.height * 3
                            && timeline.extendRowWindowAtNewEnd())
                            start()
                    }
                }
                function nearWindowNewEdge() {
                    return rowWindowSkip > 0
                           && contentY - wheelMinY() < Math.max(1, height * 3)
                }
                function extendRowWindowAtNewEnd() {
                    if (!app.timelineView
                        || !app.timelineView.extendWindowAtNewEnd)
                        return false
                    if (rowWindowSkip <= 0)
                        return false
                    const before = count
                    if (app.timelineView.extendWindowAtNewEnd(
                                windowNewEndExtendRows) !== true)
                        return false
                    let shift = 0
                    const added = count - before
                    rowColumn.forceLayout()
                    for (let r = 0; r < added; ++r) {
                        const item = itemAtViewRow(r)
                        if (item) {
                            shift += item.height
                            if (item.height <= 0)
                                ++diagWindowUnmeasuredRows
                        }
                    }
                    if (shift !== 0) {
                        setPositionY(contentY + shift, "window-extend")
                        app.timelineScroll.translateActiveMotion(shift)
                    }
                    ++diagWindowNewEndExtensions
                    newestEndInsertedRows = 0
                    captureViewAnchor()
                    if (rowWindowSkip > 0 && !rowWindowExtendTimer.running)
                        rowWindowExtendTimer.start()
                    return true
                }

                function extendRowWindowAtOldEnd() {
                    if (!app.timelineView
                        || !app.timelineView.extendWindowAtOldEnd)
                        return false
                    return app.timelineView.extendWindowAtOldEnd(
                               windowMarginRows) === true
                }

                function maybeRequestNearTop(userInitiated) {
                    if (!stickToBottom && viewAnchorId === "")
                        captureViewAnchor()
                    if (userInitiated)
                        nearTopCheckUserInitiated = true
                    if (nearTopCheckScheduled)
                        return
                    nearTopCheckScheduled = true
                    Qt.callLater(function() {
                        if (!timeline) return
                        nearTopCheckScheduled = false
                        var ui = nearTopCheckUserInitiated
                        nearTopCheckUserInitiated = false
                        if (app.currentRoomId !== "")
                            app.pagination.requestNearTop(ui)
                    })
                }

                readonly property real nearTopEnterDistance: height * 2.5
                readonly property real nearTopExitDistance: height * 3.25
                property bool nearTopArmed: true
                readonly property int nearTopApproachRowBudget: 240
                property int nearTopRowsThisApproach: 0
                readonly property int nearTopApproachRequestBudget: 24
                property int nearTopRequestsThisApproach: 0
                function distanceFromTop() { return wheelMaxY() - contentY }
                property real nearTopRequestDistance: Infinity
                function checkNearTopEdge(userInitiated) {
                    if (stickToBottom) {
                        nearTopArmed = true
                        nearTopRequestDistance = Infinity
                        nearTopRowsThisApproach = 0
                        nearTopRequestsThisApproach = 0
                        return
                    }
                    var fromTop = distanceFromTop()
                    if (fromTop <= nearTopEnterDistance) {
                        if (nearTopArmed
                            && (fromTop <= 1
                                || fromTop < nearTopRequestDistance - 1)) {
                            if (extendRowWindowAtOldEnd())
                                return
                            if (nearTopRowsThisApproach
                                    < nearTopApproachRowBudget
                                && nearTopRequestsThisApproach
                                    < nearTopApproachRequestBudget) {
                                nearTopArmed = false
                                ++nearTopRequestsThisApproach
                                if (scrollTrace)
                                    ++diagTopRequests
                                maybeRequestNearTop(userInitiated)
                            } else if (scrollTrace) {
                                ++diagTopBudgetBlocked
                            }
                        }
                        if (fromTop < nearTopRequestDistance)
                            nearTopRequestDistance = fromTop
                    } else if (fromTop >= nearTopExitDistance) {
                        nearTopArmed = true
                        nearTopRequestDistance = Infinity
                        nearTopRowsThisApproach = 0
                        nearTopRequestsThisApproach = 0
                    }
                }

                function beginExplicitTopApproach() {
                    nearTopArmed = true
                    nearTopRequestDistance = Infinity
                    nearTopRowsThisApproach = 0
                    nearTopRequestsThisApproach = 0
                    explicitTopStartRows = count
                    explicitTopCeilingLogged = false
                }
                readonly property int explicitTopRowCeiling: 600
                property int explicitTopStartRows: 0
                property bool explicitTopCeilingLogged: false
                function renewPinnedTopBudget() {
                    if (app.pagination && app.pagination.busy)
                        return
                    if (count - explicitTopStartRows >= explicitTopRowCeiling) {
                        if (!explicitTopCeilingLogged) {
                            explicitTopCeilingLogged = true
                            console.info("scroll-top-ceiling rows=" + count
                                + " addedThisGesture="
                                + (count - explicitTopStartRows)
                                + " ceiling=" + explicitTopRowCeiling
                                + " srcRows="
                                + (app.timeline ? app.timeline.count : -1))
                        }
                        return
                    }
                    nearTopRowsThisApproach = 0
                    nearTopRequestsThisApproach = 0
                }

                function cancelWheelMotion() {
                    followLatestOnArrival = false
                    app.timelineScroll.cancel()
                }

                function revealRow(row) {
                    if (!row || !row.visible || height <= 0)
                        return false
                    var a = row.mapToItem(timeline.contentItem, 0, 0).y
                    var b = row.mapToItem(timeline.contentItem, 0, row.height).y
                    var top = Math.min(a, b)
                    var bottom = Math.max(a, b)
                    var margin = 8
                    var target = contentY
                    if (bottom - top + 2 * margin > height)
                        target = bottom - height + margin
                    else if (top < contentY + margin)
                        target = top - margin
                    else if (bottom > contentY + height - margin)
                        target = bottom - height + margin
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    target = target < lo ? lo : (target > hi ? hi : target)
                    if (Math.abs(target - contentY) <= 0.5)
                        return false
                    cancelWheelMotion()
                    setPositionY(target, "keyboard")
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                    return true
                }

                property int smoothJumpViewports: 4
                property bool followLatestOnArrival: false
                onWheelAnimatingChanged: {
                    if (wheelAnimating || !followLatestOnArrival)
                        return
                    followLatestOnArrival = false
                    if (!atBottomEdge())
                        return
                    settleAtLatest()
                }
                function settleAtLatest(bounded) {
                    if (bounded === true && presentationReady
                            && !(app.pagination && app.pagination.busy)
                            && app.timelineView
                            && app.timelineView.showLatestWindow) {
                        app.timelineView.showLatestWindow(windowMinRows)
                        rowColumn.forceLayout()
                    } else {
                        releasePendingRows()
                    }
                    stickToBottom = true
                    viewAnchorId = ""
                    viewAnchorLastY = 0
                    app.pagination.saveFollowingLatest(app.currentRoomId)
                    positionViewAtLatest()
                    Qt.callLater(function() {
                        if (!timeline) return
                        positionViewAtLatest()
                        app.readReceipts.reevaluate()
                        diagNavigationShape("latest-settled")
                    })
                }

                function beginWheelTo(targetY) {
                    root.stopAutoscroll()
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    targetY = targetY < lo ? lo : (targetY > hi ? hi : targetY)
                    glideTowardsNewer = targetY < contentY - 0.5
                    app.timelineScroll.animateTo(targetY, contentY, lo, hi,
                                                 height)
                    updateStickAndPaginate()
                    if (targetY > contentY + 0.5) {
                        stickToBottom = false
                        followLatestOnArrival = false
                    }
                    scrollSettleTimer.restart()
                }

                function keyboardPage(direction) {
                    noteReaderTookControl()
                    beginWheelTo(app.timelineScroll.pageTargetY(
                        -direction * height * 0.9, contentY))
                }
                function goToEarliestLoaded() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    setPositionY(wheelMaxY(), "keyboard")
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
                property int firstUnreadPagesLeft: 0
                readonly property int maxFirstUnreadPages: 8
                function goToFirstUnread() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    firstUnreadPagesLeft = maxFirstUnreadPages
                    landOnFirstUnread()
                }
                function landOnFirstUnread() {
                    const row = app.timeline ? app.timeline.firstUnreadRow : -1
                    if (row >= 0) {
                        firstUnreadPagesLeft = 0
                        stickToBottom = false
                        if (viewRowForSourceRow(row) < 0)
                            releasePendingRows()
                        beginNavigationLanding(row, 0, true)
                        return
                    }
                    if (firstUnreadPagesLeft <= 0
                            || !app.pagination
                            || app.pagination.reachedStart) {
                        firstUnreadPagesLeft = 0
                        root.firstUnreadHuntFailed = true
                        return
                    }
                    --firstUnreadPagesLeft
                    app.pagination.requestNearTop(true)
                    firstUnreadRetryTimer.restart()
                }
                function goToLatest() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    diagNavigationShape("latest-before")
                    var lo = wheelMinY()
                    var distance = contentY - lo
                    if (height > 0 && distance > 0 && rowWindowSkip === 0
                        && distance <= height * smoothJumpViewports) {
                        followLatestOnArrival = true
                        glideTowardsNewer = true
                        app.timelineScroll.animateTo(lo, contentY, lo,
                                                     wheelMaxY(), height)
                        scrollSettleTimer.restart()
                        return
                    }
                    if (app.trimHistoryAndJumpToLive()) {
                        stickToBottom = true
                        app.pagination.saveFollowingLatest(app.currentRoomId)
                        return
                    }
                    settleAtLatest()
                }
                activeFocusOnTab: true
                function runKeyAction(actionId) {
                    switch (actionId) {
                    case "timeline.pageUp":      keyboardPage(-1); return true
                    case "timeline.pageDown":    keyboardPage(1); return true
                    case "timeline.jumpToStart": goToEarliestLoaded(); return true
                    case "timeline.jumpToEnd":   goToLatest(); return true
                    }
                    return false
                }
                Keys.onShortcutOverride: (event) => {
                    if (navigationAction(event.key, event.modifiers) !== "")
                        event.accepted = true
                }
                Keys.onPressed: (event) => {
                    var action = navigationAction(event.key, event.modifiers)
                    if (action !== "" && runNavigationAction(action)) {
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageUp
                            && (event.modifiers & Qt.ShiftModifier)) {
                        goToFirstUnread()
                        event.accepted = true
                        return
                    }
                    var keyAction = app.shortcuts.timelineActionForKey(
                        event.key, event.modifiers)
                    if (keyAction !== "" && runKeyAction(keyAction)) {
                        event.accepted = true
                        return
                    }
                    switch (event.key) {
                    case Qt.Key_Home:
                        goToEarliestLoaded(); event.accepted = true; break
                    case Qt.Key_End:
                        goToLatest(); event.accepted = true; break
                    case Qt.Key_Space:
                        keyboardPage((event.modifiers & Qt.ShiftModifier) ? -1 : 1)
                        event.accepted = true; break
                    default:
                        event.accepted = false
                    }
                }
                // Clicking the timeline gives it keyboard focus without
                // stealing focus while typing. TapHandlers are non-exclusive,
                // so a tap that opened a popup from a row (the read-receipt
                // list, a profile card) reaches this one too, after the row's:
                // leave the focus with the popup, or its Escape goes to the
                // shell and closes the room.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: {
                        for (var it = timeline.Window.activeFocusItem; it;
                             it = it.parent) {
                            if (it === timeline.Overlay.overlay)
                                return
                        }
                        messageComposer.cancelPendingFocus()
                        timeline.forceActiveFocus()
                    }
                }

                Connections {
                    target: app.timelineScroll
                    function onWheelPositionChanged(y) {
                        var lo = timeline.wheelMinY()
                        if (y <= lo && timeline.rowWindowSkip > 0
                            && timeline.extendRowWindowAtNewEnd()) {
                            return
                        }
                        var hi = timeline.wheelMaxY()
                        var clamped = y < lo ? lo : (y > hi ? hi : y)
                        timeline.setPositionY(clamped, "wheel")
                        if (clamped !== y)
                            app.timelineScroll.notifyBoundReached(clamped)
                    }
                    function onWheelMotionSettled() {
                        timeline.updateStickAndPaginate()
                        timeline.captureViewAnchor()
                        scrollSettleTimer.restart()
                    }
                }

                Timer {
                    id: scrollSettleTimer
                    objectName: "scrollSettleTimer"
                    interval: 250
                    onTriggered: {
                        rowWindowExtendTimer.stop()
                        timeline.refreshMediaBand()
                        timeline.updateStickAndPaginate()
                        timeline.saveRoomPosition()
                        timeline.captureViewAnchor()
                        timeline.applyRowWindow()
                        if (!timeline.stickToBottom
                            && !app.pagination.reachedStart
                            && timeline.distanceFromTop()
                               <= timeline.nearTopEnterDistance)
                            timeline.nearTopArmed = true
                        timeline.diagFlushGesture()
                    }
                }

                WheelHandler {
                    id: timelineWheelHandler
                    objectName: "timelineWheelHandler"
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        timeline.pendingWindowSkip = -1
                        rowWindowTrimTimer.stop()
                        var notchStartMs = timeline.scrollTrace ? Date.now() : 0
                        root.stopAutoscroll()
                        timeline.noteReaderTookControl()
                        var towardsOlder = event.pixelDelta.y !== 0
                                ? event.pixelDelta.y > 0
                                : event.angleDelta.y > 0
                        if (!towardsOlder && timeline.nearWindowNewEdge())
                            timeline.extendRowWindowAtNewEnd()
                        var canMove = timeline.wheelCanMove(towardsOlder)
                        var minY = timeline.wheelMinY()
                        var maxY = timeline.wheelMaxY()
                        if (towardsOlder && !canMove && maxY <= minY + 0.5
                                && app.pagination && !app.pagination.reachedStart)
                            app.pagination.requestNearTop(true)
                        var continuousSource = event.phase !== Qt.NoScrollPhase
                        if (event.pixelDelta.y !== 0
                                || (continuousSource && event.angleDelta.y !== 0)) {
                            timeline.cancelWheelMotion()
                            timeline.setPositionY(app.timelineScroll.pixelTargetY(
                                -event.pixelDelta.y, timeline.contentY,
                                minY, maxY), "pixel")
                            timeline.updateStickAndPaginate(canMove)
                            if (towardsOlder && canMove)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0
                                   && !timeline.smoothScrollingEnabled) {
                            timeline.cancelWheelMotion()
                            var per = app.timelineScroll.notchDistance(
                                timeline.height)
                            var jump = (event.angleDelta.y / 120.0) * per
                            timeline.setPositionY(Math.max(
                                minY, Math.min(maxY, timeline.contentY + jump)),
                                "wheel")
                            timeline.updateStickAndPaginate(canMove)
                            if (event.angleDelta.y > 0 && canMove)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0) {
                            timeline.glideTowardsNewer = event.angleDelta.y < 0
                            app.timelineScroll.wheelNotch(
                                -event.angleDelta.y, timeline.contentY,
                                minY, maxY, timeline.height)
                            timeline.updateStickAndPaginate(canMove)
                            if (event.angleDelta.y > 0 && canMove) {
                                timeline.stickToBottom = false
                                timeline.followLatestOnArrival = false
                            }
                            timeline.diagNoteEvent(false)
                            scrollSettleTimer.restart()
                        }
                        timeline.diagNoteNotchCost(notchStartMs)
                        event.accepted = true
                    }
                }

                onDragStarted: { cancelWheelMotion(); noteReaderTookControl() }
                onFlickStarted: { cancelWheelMotion(); noteReaderTookControl() }

                Component.onDestruction: cancelWheelMotion()

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
                Binding {
                    target: app
                    property: "activeRoomAtLatest"
                    value: timeline.visible && timeline.presentationReady
                           && timeline.Window.active === true
                           && timeline.stickToBottom
                }
                Binding {
                    target: app
                    property: "activeRoomHydrating"
                    value: timeline.visible && !timeline.presentationReady
                           && timeline.Window.active === true
                }

                property bool viewportFillCheckScheduled: false
                readonly property int maxViewportFillRetries: 8
                readonly property int maxInvisibleFillRetries: 12
                readonly property int maxViewportFillRows: 240
                readonly property int maxEmptyFillPages: 60
                property int viewportFillInvisibleRetries: 0
                property int viewportFillEmptyPages: 0
                property int viewportFillRetries: 0
                property real viewportFillLastHeight: -1
                property int viewportFillLastRows: -1
                property int viewportFillLastEmptyPages: -1
                Timer {
                    id: viewportFillRetryTimer
                    interval: 250
                    repeat: false
                    onTriggered: timeline.maybeFillViewport()
                }
                function maybeFillViewport() {
                    if (viewportFillCheckScheduled)
                        return
                    viewportFillCheckScheduled = true
                    Qt.callLater(function() {
                        if (!timeline) return
                        viewportFillCheckScheduled = false
                        if (app.currentRoomId === ""
                            || contentHeight >= height) {
                            viewportFillRetries = 0
                            viewportFillInvisibleRetries = 0
                            viewportFillEmptyPages = 0
                            viewportFillLastHeight = -1
                            viewportFillLastRows = -1
                            viewportFillLastEmptyPages = -1
                            viewportFillRetryTimer.stop()
                            return
                        }
                        if (app.pagination.busy) {
                            viewportFillRetryTimer.restart()
                            return
                        }
                        function declineReason() {
                            if (app.pagination.reachedStart) return "reachedStart"
                            if (app.pagination.fillStopped) return "fillStopped"
                            if (app.pagination.failed) return "failed"
                            if ((app.timeline ? app.timeline.count : 0)
                                    >= maxViewportFillRows)
                                return "rowBudget"
                            if (viewportFillRetries >= maxViewportFillRetries
                                    && (app.timeline ? app.timeline.count : 0) > 0)
                                return "noProgressBudget"
                            if (viewportFillInvisibleRetries
                                >= maxInvisibleFillRetries)
                                return "invisibleBudget"
                            if (viewportFillEmptyPages >= maxEmptyFillPages)
                                return "emptyPageBudget"
                            return ""
                        }
                        var loadedRows = app.timeline ? app.timeline.count : 0
                        var emptyPages = app.pagination
                                ? app.pagination.emptyFillPages : 0
                        var grewHeight = viewportFillLastHeight >= 0
                                && contentHeight > viewportFillLastHeight + 1
                        var grewRows = viewportFillLastRows >= 0
                                && loadedRows > viewportFillLastRows
                        var walkedFilteredHistory =
                                viewportFillLastEmptyPages >= 0
                                && emptyPages > viewportFillLastEmptyPages
                        if (grewHeight) {
                            viewportFillRetries = 0
                            viewportFillInvisibleRetries = 0
                            viewportFillEmptyPages = 0
                        } else if (grewRows) {
                            viewportFillRetries = 0
                            viewportFillEmptyPages = 0
                            ++viewportFillInvisibleRetries
                        } else if (walkedFilteredHistory) {
                            viewportFillRetries = 0
                            ++viewportFillEmptyPages
                        }
                        viewportFillLastHeight = contentHeight
                        viewportFillLastRows = loadedRows
                        viewportFillLastEmptyPages = emptyPages
                        var decline = declineReason()
                        if (decline !== "") {
                            if (scrollTrace) {
                                console.log("fill-declined reason=" + decline
                                    + " rows=" + loadedRows
                                    + " contentH=" + Math.round(contentHeight)
                                    + " height=" + Math.round(height)
                                    + " noProgress=" + viewportFillRetries
                                    + " invisible=" + viewportFillInvisibleRetries
                                    + " emptyPages=" + viewportFillEmptyPages)
                            }
                            return
                        }
                        ++viewportFillRetries
                        app.pagination.requestViewportFill()
                        if (scrollTrace) {
                            console.log("fill-requested rows=" + loadedRows
                                + " contentH=" + Math.round(contentHeight)
                                + " height=" + Math.round(height)
                                + " grewRows=" + (grewRows ? 1 : 0)
                                + " grewHeight=" + (grewHeight ? 1 : 0)
                                + " filtered=" + (walkedFilteredHistory ? 1 : 0)
                                + " noProgress=" + viewportFillRetries
                                + " invisible=" + viewportFillInvisibleRetries
                                + " emptyPages=" + viewportFillEmptyPages)
                        }
                        viewportFillRetryTimer.restart()
                    })
                }
                onContentHeightChanged: {
                    presentationGeometryStale = false
                    if (mediaBandOpen && !userScrollActive)
                        mediaBandGrowth.start()
                    scheduleVisibleRowRange()
                    maybeFillViewport()
                    recomputePresentationReady()
                    if (stickToBottom) {
                        if (!moving && !wheelAnimating && count > 0)
                            scrollToEndTimer.restart()
                    } else {
                        maintainViewAnchorCoalesced()
                    }
                }
                onHeightChanged: {
                    refreshMediaBand()
                    maybeFillViewport()
                    recomputePresentationReady()
                    if (stickToBottom) {
                        if (count > 0)
                            scrollToEndTimer.restart()
                    } else {
                        maintainViewAnchorCoalesced()
                    }
                }

                Connections {
                    target: app.pagination
                    function onPaginationCompleted(insertedCount, reachedStart,
                                                   willContinue) {
                        if (insertedCount > 0 && !timeline.stickToBottom
                                && timeline.distanceFromTop()
                                   <= timeline.nearTopEnterDistance)
                            timeline.nearTopRowsThisApproach += insertedCount
                        if (insertedCount <= 0 || reachedStart || willContinue)
                            return
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance =
                                timeline.distanceFromTop()
                    }
                    function onTargetLocated(row, pixelOffset, highlight) {
                        if (!highlight && timeline.readerControlledSinceReset) {
                            ++timeline.diagNavigationAbandoned
                            timeline.releaseRestoreHold()
                            return
                        }
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
                        var locatedId = app.timeline
                                        ? app.timeline.stableIdAt(row) : ""
                        var epoch = timeline.navigationEpoch
                        Qt.callLater(function() {
                            if (!timeline) return
                            if (timeline.navigationEpoch !== epoch) {
                                ++timeline.diagNavigationAbandoned
                                if (!highlight)
                                    timeline.releaseRestoreHold()
                                return
                            }
                            timeline.releasePendingRows()
                            timeline.beginNavigationLanding(row, pixelOffset,
                                                            highlight,
                                                            locatedId)
                        })
                    }
                    function onRestoreLatestRequested() {
                        timeline.releaseRestoreHold()
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
                        timeline.stickToBottom = true
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorLastY = 0
                        scrollToEndTimer.restart()
                    }
                }
                Connections {
                    target: app.composer
                    function onOwnMessageSent() {
                        if (!timeline.stickToBottom)
                            timeline.goToLatest()
                    }
                }
                Connections {
                    target: app.settings
                    function onShowRoomActivityChanged() {
                        if (timeline.stickToBottom) {
                            scrollToEndTimer.restart()
                            return
                        }
                        timeline.captureViewAnchor()
                        Qt.callLater(function() {
                            if (!timeline) return
                            timeline.maintainViewAnchor()
                            timeline.maybeFillViewport()
                        })
                    }
                }

                property var openRowMenu: null
                property string positionWriter: ""
                function setPositionY(y, writer) {
                    var previous = positionWriter
                    positionWriter = writer
                    contentY = y
                    positionWriter = previous
                }
                property real recorderLastY: 0
                property int diagScrollIncidents: 0
                function recordPositionWrite() {
                    var oldY = recorderLastY
                    recorderLastY = contentY
                    var writer = positionWriter !== "" ? positionWriter
                               : dragging || flicking ? "native"
                               : scrollBarHeld ? "bar-drag"
                               : userScrollActive ? "input" : "unattributed"
                    var flags = (userScrollActive ? 1 : 0)
                              | (stickToBottom ? 2 : 0)
                              | (wheelAnimating ? 4 : 0)
                              | (scrollSettleTimer.running ? 8 : 0)
                              | (navigationPendingId !== "" ? 16 : 0)
                              | (restoreHoldsGate ? 32 : 0)
                              | (scrollBarHeld ? 64 : 0)
                              | (dragging ? 128 : 0)
                    var minY = wheelMinY()
                    var maxY = wheelMaxY()
                    app.timelineScroll.recordScroll(
                        writer, oldY, contentY, minY, maxY, height,
                        contentHeight, count, rowWindowSkip, flags,
                        viewAnchorId, viewAnchorOffset,
                        viewAnchorId !== "" ? viewAnchorKind : "",
                        viewAnchorId !== "" && viewAnchorRow >= 0
                            ? count - 1 - viewAnchorRow : -1)
                    var jump = Math.abs(contentY - oldY)
                    if (jump > height && contentY >= maxY - 2
                            && maxY - minY > height
                            && writer !== "navigation" && writer !== "keyboard"
                            && writer !== "bar-drag" && writer !== "bar-top-hold"
                            && writer !== "window-trim"
                            && writer !== "window-extend") {
                        ++diagScrollIncidents
                        app.timelineScroll.reportScrollIncident("oldest-bound")
                    }
                }
                onContentYChanged: {
                    if (app.timelineScroll.scrollRecorderEnabled)
                        recordPositionWrite()
                    if (openRowMenu) {
                        var stale = openRowMenu
                        openRowMenu = null
                        stale.close()
                    }
                    mediaBandSettle.restart()
                    scheduleVisibleRowRange()
                    if (!userScrollActive) return
                    stickToBottom = atBottomEdge()
                    checkNearTopEdge(true)
                }
                onCountChanged: {
                    scheduleVisibleRowRange()
                    recomputePresentationReady()
                    if (stickToBottom) scrollToEndTimer.restart()
                }
                onMovementEnded: {
                    refreshMediaBand()
                    stickToBottom = atBottomEdge()
                    saveRoomPosition()
                    captureViewAnchor()
                }
                Component.onCompleted: {
                    refreshMediaBand()
                    scrollToEndTimer.restart()
                    maybeFillViewport()
                    if (app.currentRoomId !== "" && count > 0)
                        presentationGuard.restart()
                    recomputePresentationReady()
                }
                Connections {
                    target: app.timeline
                    function onModelAboutToBeReset() {
                        timeline.presentationResetHadRows = timeline.count > 0
                    }
                    function onModelReset() {
                        timeline.cancelWheelMotion()
                        timeline.refreshMediaBand()
                        timeline.mediaBandOpen = true
                        root.stopAutoscroll()
                        root.closeRowAnchoredSurfaces()
                        timeline.stickToBottom = true
                        timeline.pinnedActionsKey = ""
                        timeline.emojiPickerOpen = false
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorOffset = 0
                        timeline.viewAnchorLastY = 0
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance = Infinity
                        timeline.nearTopRowsThisApproach = 0
                        timeline.nearTopRequestsThisApproach = 0
                        timeline.expandedStateGroups = ({})
                        timeline.firstUnreadSeen = false
                        timeline.firstUnreadSeenId = ""
                        timeline.viewportNewestRow = 0
                        timeline.viewportOldestRow = -1
                        timeline.viewportFillRetries = 0
                        timeline.viewportFillInvisibleRetries = 0
                        timeline.viewportFillLastHeight = -1
                        timeline.viewportFillLastRows = -1
                        viewportFillRetryTimer.stop()
                        timeline.navigationPendingRow = -1
                        timeline.navigationPendingId = ""
                        navigationLandingTimer.stop()
                        timeline.readerControlledSinceReset = false
                        timeline.presentationReady = false
                        timeline.presentationResetPending = true
                        timeline.presentationGeometryStale =
                                timeline.presentationResetHadRows
                        presentationGuard.restart()
                        timeline.restoreHoldsGate =
                                app.pagination.hasHistoricalReturn(
                                    app.currentRoomId,
                                    app.settings.conversationReturnMode)
                        if (timeline.restoreHoldsGate)
                            restoreHoldGuard.restart()
                        else
                            restoreHoldGuard.stop()
                        var resetEpoch = ++timeline.resetEpoch
                        var readerEpoch = timeline.navigationEpoch
                        Qt.callLater(function() {
                            if (!timeline || !app
                                    || timeline.resetEpoch !== resetEpoch
                                    || timeline.navigationEpoch !== readerEpoch)
                                return
                            app.pagination.restoreConversation(
                                app.currentRoomId,
                                app.settings.conversationReturnMode)
                        })
                        Qt.callLater(function() {
                            if (timeline)
                                timeline.maybeFillViewport()
                        })
                        presentationResetSettledTimer.restart()
                    }
                }

                onAtYEndChanged: {
                    if (atYEnd)
                        maybeRequestNearTop(false)
                }

                // The loading/failure indicator is a top overlay
                // (paginationHeader), not list content, so toggling it never
                // changes timeline geometry.

            }

            AppScrollBar {
                id: timelineScrollBar
                objectName: "timelineScrollBar"
                orientation: Qt.Vertical
                policy: ScrollBar.AsNeeded
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                visible: timeline.contentHeight > timeline.height
                readonly property real span:
                    Math.max(1, timeline.wheelMaxY() - timeline.wheelMinY())
                size: Math.max(0.02,
                               Math.min(1, timeline.height
                                           / Math.max(1, timeline.contentHeight)))
                // Tracking lives in the Binding below: ScrollBar assigns
                // `position` imperatively while dragged, which would destroy a
                // plain binding.
                // A handle drag is its own approach to the top: it starts with
                // a fresh row/request budget and an armed latch, or a reader
                // who already spent one on the wheel is never served.
                onPressedChanged: {
                    if (pressed) {
                        timeline.diagNoteInput("scrollbar")
                        timeline.beginExplicitTopApproach()
                        timeline.cancelWheelMotion()
                        timeline.noteReaderTookControl()
                    } else {
                        scrollSettleTimer.restart()
                    }
                }
                function handleFraction() {
                    return (1 - size) > 0 ? position / (1 - size) : 0
                }
                Timer {
                    id: scrollBarTopHold
                    objectName: "timelineScrollBarTopHold"
                    interval: 120
                    repeat: true
                    running: timelineScrollBar.pressed
                    onTriggered: {
                        if (timelineScrollBar.handleFraction() > 0.001)
                            return
                        if (timeline.scrollTrace)
                            ++timeline.diagScrollbarTopHolds
                        timeline.renewPinnedTopBudget()
                        timeline.setPositionY(timeline.wheelMaxY(), "bar-top-hold")
                        timeline.updateStickAndPaginate()
                    }
                }
                onPositionChanged: {
                    if (!pressed)
                        return
                    var frac = handleFraction()
                    timeline.diagNoteInput("scrollbar")
                    timeline.cancelWheelMotion()
                    timeline.noteReaderTookControl()
                    timeline.setPositionY(timeline.wheelMaxY() - frac * span, "bar-drag")
                    timeline.updateStickAndPaginate()
                }
            }

            Binding {
                target: timelineScrollBar
                property: "position"
                when: !timelineScrollBar.pressed
                restoreMode: Binding.RestoreNone
                value: {
                    if (!timelineScrollBar.visible)
                        return 0
                    var usable = 1 - timelineScrollBar.size
                    var frac = (timeline.wheelMaxY() - timeline.contentY)
                               / timelineScrollBar.span
                    return Math.max(0, Math.min(usable, frac * usable))
                }
            }

            Item {
                objectName: "paginationHeader"
                anchors.top: parent.top
                anchors.topMargin: jumpToFirstUnreadButton.visible
                                   ? jumpToFirstUnreadButton.y
                                     + jumpToFirstUnreadButton.height
                                     + AppTheme.spacing4
                                   : 0
                anchors.left: parent.left
                anchors.right: parent.right
                z: 15
                clip: true
                height: app.pagination.presentationState
                        === PaginationController.Hidden ? 0 : 32
                visible: app.currentRoomId !== ""
                         && app.pagination.presentationState
                            !== PaginationController.Hidden
                AppProgressBar {
                    objectName: "paginationLoadingBar"
                    visible: app.pagination.presentationState
                             === PaginationController.Loading
                    indeterminate: visible
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    implicitHeight: 3
                    height: 3
                    Accessible.name: qsTr("Loading older messages…")
                }
                Rectangle {
                    visible: app.pagination.presentationState
                             !== PaginationController.Loading
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
                        spacing: AppTheme.spacing6
                        AppBusyIndicator {
                            size: 16
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
                                   ? AppTheme.danger : AppTheme.textMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.weight: AppTheme.weightMedium
                        }
                        AbstractButton {
                            id: paginationRetryButton
                            objectName: "paginationRetryButton"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: app.pagination.presentationState
                                     === PaginationController.Failed
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            implicitWidth: paginationRetryLabel.implicitWidth
                                           + AppTheme.spacing8
                            implicitHeight: paginationRetryLabel.implicitHeight
                                            + AppTheme.spacing4
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Retry loading older messages")
                            onClicked: app.pagination.retry()
                            background: Rectangle {
                                radius: AppTheme.radiusSm
                                color: paginationRetryButton.hovered
                                       ? AppTheme.buttonGhostHover
                                       : "transparent"
                                border.width:
                                    paginationRetryButton.visualFocus ? 2 : 0
                                border.color: AppTheme.focusRing
                            }
                            Label {
                                id: paginationRetryLabel
                                anchors.centerIn: parent
                                text: qsTr("Retry")
                                color: AppTheme.link
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightMedium
                                font.underline: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }
                }
            }

            readonly property bool spaceViewActive:
                app.currentRoomId === ""
                && app.spaces && app.spaces.activeSpaceId.length > 0
                && app.spaces.activeSpaceId.charAt(0) === "!"

            HomePane {
                objectName: "homePane"
                anchors.fill: parent
                visible: app.currentRoomId === "" && !parent.spaceViewActive
                onNewMessageRequested: root.newConversationRequested("dm", undefined)
                onCreateRoomRequested: root.newConversationRequested("room", undefined)
                onCreateSpaceRequested: root.newConversationRequested("space", undefined)
                onDiscoverRequested: root.discoverRoomsRequested()
            }

            MiddleClickScroller {
                id: middleClickScroller
                objectName: "timelineMiddleClickScroller"
                anchors.fill: parent
                z: middleClickScroller.latched ? 30 : 2
                visible: app.currentRoomId !== "" && timeline.presentationReady
                view: timeline
                inverted: true
                trace: timeline.scrollTrace
                property real lastMediaBandMs: 0
                minYFunc: function() { return timeline.wheelMinY() }
                maxYFunc: function() { return timeline.wheelMaxY() }
                onActiveChanged: if (active) {
                    timeline.cancelWheelMotion()
                    timeline.noteReaderTookControl()
                    timeline.beginExplicitTopApproach()
                }
                onScrolled: {
                    timeline.diagNoteInput("autoscroll")
                    var travel = middleClickScroller.pointerY
                                 - middleClickScroller.anchorY
                    var steering = Math.abs(travel) > middleClickScroller.deadZone
                    if (steering && travel < 0
                            && timeline.distanceFromTop() <= 1)
                        timeline.renewPinnedTopBudget()
                    if (steering && travel > 0 && timeline.nearWindowNewEdge())
                        timeline.extendRowWindowAtNewEnd()
                    var now = Date.now()
                    if (now - middleClickScroller.lastMediaBandMs >= 500) {
                        middleClickScroller.lastMediaBandMs = now
                        timeline.refreshMediaBand()
                    }
                    timeline.updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
            }

            Loader {
                objectName: "spaceHomeLoader"
                anchors.fill: parent
                active: parent.spaceViewActive
                visible: active
                sourceComponent: spaceHomeComponent
            }

            Item {
                objectName: "timelineLoadingSurface"
                anchors.fill: parent
                visible: !timeline.presentationReady
                Row {
                    anchors.centerIn: parent
                    spacing: AppTheme.spacingS
                    AppBusyIndicator {
                        size: 18
                        anchors.verticalCenter: parent.verticalCenter
                        running: !timeline.presentationReady
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Loading conversation…")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightMedium
                    }
                }
            }

            Item {
                objectName: "timelineEmptyState"
                anchors.fill: parent
                visible: app.currentRoomId !== "" && timeline.count === 0
                         && timeline.presentationReady
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - AppTheme.spacing24 * 2, 380)
                    spacing: AppTheme.spacing12
                    Avatar {
                        Layout.alignment: Qt.AlignHCenter
                        size: 64
                        squareRadius: AppTheme.radiusLg
                        name: root.currentRoom.name || app.currentRoomId
                        mxc: root.inviteAvatarWithheld
                             ? "" : (root.currentRoom.avatarUrl || "")
                        colorKey: root.currentRoom.identityColorKey
                                  || app.currentRoomId
                        circle: root.currentRoom.isDirect === true
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: root.currentRoom.name || qsTr("No messages yet")
                        textFormat: Text.PlainText
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textDisplay)
                        font.weight: AppTheme.weightDisplay
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        text: root.currentRoom.isDirect === true
                              ? qsTr("No messages here yet. Say hello.")
                              : qsTr("No messages here yet. Start the conversation.")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: AppTheme.spacingXS
                        visible: root.currentRoom.encrypted === true
                        Icon {
                            name: "lock"
                            size: 14
                            color: AppTheme.textMuted
                        }
                        Label {
                            text: qsTr("Messages are end-to-end encrypted")
                            color: AppTheme.textMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        }
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: AppTheme.spacingS
                        spacing: AppTheme.spacingS
                        AppButton {
                            objectName: "emptyRoomInviteButton"
                            kind: "primary"
                            text: qsTr("Invite people")
                            visible: app.roomInfo
                                     && app.roomInfo.supported
                                     && app.roomInfo.roomId === app.currentRoomId
                                     && app.roomInfo.canInvite
                            onClicked: roomInviteDialog.openFor(app.currentRoomId)
                        }
                        AppButton {
                            objectName: "emptyRoomInfoButton"
                            text: qsTr("Room information")
                            visible: app.roomInfo && app.roomInfo.supported
                            onClicked: root.toggleRoomInfo()
                        }
                    }
                }
            }

            AbstractButton {
                id: jumpToFirstUnreadButton
                enabled: !root.invitePreview
                objectName: "jumpToFirstUnreadButton"
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingM
                x: (app.pagination && app.pagination.reachedStart)
                   ? parent.width - width - AppTheme.spacingM
                   : (parent.width - width) / 2
                visible: app.currentRoomId !== "" && app.timeline
                         && (timeline.firstUnreadLoaded
                             ? !timeline.firstUnreadOnScreen
                               && !(timeline.firstUnreadPassed
                                    && timeline.stickToBottom
                                    && timeline.rowWindowSkip === 0)
                             : (root.openedWithUnread
                                && !root.firstUnreadHuntFailed
                                && !!app.pagination
                                && app.pagination.moreHistory))
                z: 20
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                implicitHeight: 30
                implicitWidth: firstUnreadPillRow.implicitWidth + 24
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Jump to first unread message")
                ToolTip.text: qsTr("Go to where you stopped reading")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: timeline.goToFirstUnread()

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: jumpToFirstUnreadButton.down
                           ? Qt.darker(AppTheme.unreadBadge, 1.2)
                           : jumpToFirstUnreadButton.hovered
                             ? Qt.lighter(AppTheme.unreadBadge, 1.1)
                             : AppTheme.unreadBadge
                }
                Row {
                    id: firstUnreadPillRow
                    anchors.centerIn: parent
                    spacing: AppTheme.spacingXS
                    Icon {
                        objectName: "jumpToFirstUnreadIcon"
                        anchors.verticalCenter: parent.verticalCenter
                        name: timeline.firstUnreadBelow ? "expand_more"
                                                        : "expand_less"
                        size: 18
                        color: AppTheme.accentText
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("First unread")
                        color: AppTheme.accentText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightBold
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: (parent.height + 6) / 2
                    color: "transparent"
                    border.color: AppTheme.focusRing
                    border.width: 2
                    visible: jumpToFirstUnreadButton.visualFocus
                }
            }

            Timer {
                id: firstUnreadNoteTimer
                interval: 5000
                onTriggered: root.firstUnreadNoteShown = false
            }
            Rectangle {
                id: firstUnreadFailedNote
                objectName: "firstUnreadFailedNote"
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingM
                readonly property bool atStart:
                    app.pagination && app.pagination.reachedStart
                x: atStart ? parent.width - width - AppTheme.spacingM
                           : (parent.width - width) / 2
                visible: root.firstUnreadNoteShown
                z: 20
                implicitHeight: 30
                width: Math.min(atStart ? parent.width * 0.35
                                        : parent.width - 2 * AppTheme.spacingM,
                                failedNoteLabel.implicitWidth + 24)
                height: implicitHeight
                radius: height / 2
                color: AppTheme.unreadBadge
                Accessible.role: Accessible.StaticText
                Accessible.name: failedNoteLabel.text
                Label {
                    id: failedNoteLabel
                    anchors.centerIn: parent
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: qsTr("Couldn't find the first unread message")
                    color: AppTheme.accentText
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightBold
                }
            }

            AbstractButton {
                id: jumpToLatestButton
                enabled: !root.invitePreview
                objectName: "jumpToLatestButton"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: AppTheme.spacingM + 12
                anchors.bottomMargin: AppTheme.spacingM + 8
                visible: app.currentRoomId !== ""
                         && ((!timeline.stickToBottom
                              && !timeline.followLatestOnArrival)
                             || timeline.rowWindowSkip > 0)
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
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightBold
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

            Rectangle {
                objectName: "jumpProgress"
                visible: app.pagination.navigating
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: jumpToLatestButton.visible
                                ? jumpToLatestButton.top : parent.bottom
                anchors.bottomMargin: AppTheme.spacingS
                implicitWidth: jumpProgressRow.implicitWidth + AppTheme.spacing12 * 2
                implicitHeight: jumpProgressRow.implicitHeight + AppTheme.spacing6 * 2
                width: implicitWidth
                height: implicitHeight
                color: AppTheme.cardElevated
                border.color: AppTheme.borderStrong
                border.width: 1
                radius: AppTheme.radiusPill
                z: 21
                Row {
                    id: jumpProgressRow
                    anchors.centerIn: parent
                    spacing: AppTheme.spacing8
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Loading older messages…")
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightMedium
                    }
                    ToolButton {
                        objectName: "jumpProgressCancel"
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Cancel")
                        onClicked: app.pagination.cancelNavigation()
                    }
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
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightMedium
                leftPadding: AppTheme.spacing12
                rightPadding: AppTheme.spacing12
                topPadding: AppTheme.spacing6
                bottomPadding: AppTheme.spacing6
                z: 21
                background: Rectangle {
                    color: AppTheme.cardElevated
                    border.color: AppTheme.borderStrong
                    border.width: 1
                    radius: AppTheme.radiusPill
                }
            }

            InvitePreviewCard {
                id: invitePreviewCard
                anchors.fill: parent
                z: 90
                visible: root.invitePreview
                roomId: app.currentRoomId
                roomName: root.currentRoom.name || ""
                topic: root.currentRoom.topic || ""
                inviter: root.currentRoom.inviter || ""
                avatarUrl: root.currentRoom.avatarUrl || ""
                showAvatar: !!app.accountPrefs
                            && app.accountPrefs.inviteAvatarsAllowed
                colorKey: root.currentRoom.identityColorKey || ""
                isDirect: root.currentRoom.isDirect === true
                isSpace: root.currentRoom.isSpace === true
                pending: root.currentRoom.invitePending === true
                errorText: root.currentRoom.inviteError || ""
                onAcceptRequested: app.roomList.acceptInvite(app.currentRoomId)
                onDeclineRequested: app.roomList.rejectInvite(app.currentRoomId)
            }

            ContextView {
                id: contextView
                anchors.fill: parent
                z: 100
                currentRoom: root.currentRoom
                openImage: function(mediaKey, httpUrl) {
                    imageViewer.openFor(mediaKey || "", httpUrl)
                }
                openArtwork: function(source, title) {
                    imageViewer.openArtwork(source, title)
                }
                saveMedia: function(mediaKey, filename, mime, ask) {
                    root.startDownload(mediaKey, filename, mime, ask, "context")
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: app.currentRoomId !== ""
                ? Math.max(typingLabel.implicitHeight, typingMetrics.height) + 6
                : 0
            visible: app.currentRoomId !== ""
            color: paneGround.hasGradient ? "transparent" : AppTheme.timelineBackground
            FontMetrics {
                id: typingMetrics
                font.family: AppTheme.uiFont
                font.italic: true
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
            Label {
                textFormat: Text.PlainText
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                opacity: text.length > 0 ? 1 : 0
                Accessible.ignored: text.length === 0
                elide: Text.ElideRight
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.italic: true
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 0
            z: 5
            Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: saveResult.text.length > 0
            height: saveResult.implicitHeight + AppTheme.spacing8
            color: AppTheme.cardElevated
            Row {
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacingXS
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: saveResult.ok ? "check_circle" : "error"
                    size: 14
                    color: saveResult.ok ? AppTheme.success : AppTheme.danger
                }
                Label {
                    id: saveResult
                    property bool ok: true
                    anchors.verticalCenter: parent.verticalCenter
                    color: ok ? AppTheme.success : AppTheme.danger
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightMedium
                    Timer {
                        id: saveResultTimer
                        interval: 5000
                        onTriggered: saveResult.text = ""
                    }
                }
            }
            }
        }

        BulkRedactionBar {
            Layout.fillWidth: true
            timelineModel: app.timeline
        }

        Rectangle {
            objectName: "messageSelectionBar"
            visible: app.forward.selecting === true
            Layout.fillWidth: true
            implicitHeight: visible ? selectionRow.implicitHeight
                                      + AppTheme.spacing8 * 2 : 0
            color: AppTheme.surface
            RowLayout {
                id: selectionRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: app.forward.selectedCount > 0
                          ? qsTr("%n message(s) selected", "",
                                 app.forward.selectedCount)
                          : qsTr("Tap messages to select them")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                }
                Label {
                    visible: app.forward.error.length > 0
                    textFormat: Text.PlainText
                    text: app.forward.error
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                }
                AppButton {
                    text: qsTr("Cancel")
                    kind: "ghost"
                    size: "sm"
                    onClicked: app.forward.cancelSelecting()
                }
                AppButton {
                    objectName: "forwardSelectedButton"
                    text: qsTr("Forward…")
                    kind: "primary"
                    size: "sm"
                    enabled: app.forward.selectedCount > 0
                    onClicked: forwardSelectionDialog.openDialog()
                }
            }
            Shortcut {
                sequence: "Escape"
                enabled: app.forward.selecting === true
                         && app.currentScreen === 1
                         && !middleClickScroller.active
                onActivated: app.forward.cancelSelecting()
            }
        }
        ForwardSelectionDialog { id: forwardSelectionDialog }

        MessageComposerBar {
            id: messageComposer
            objectName: "messageComposer"
            Layout.fillWidth: true
            visible: app.currentRoomId !== "" && !app.eventContext.active
                     && !root.invitePreview
            onTimelineKeyRequested: (actionId) => timeline.runKeyAction(actionId)
        }
        Connections {
            target: app.eventContext
            function onStateChanged() {
                if (!app.eventContext.active && app.currentRoomId !== "")
                    Qt.callLater(function() {
                        if (messageComposer)
                            messageComposer.focusEditor()
                    })
            }
            function onLatestRequested() {
                if (app.currentRoomId !== "")
                    contextLatestTimer.restart()
            }
        }
        Timer {
            id: contextLatestTimer
            interval: 0
            repeat: false
            onTriggered: {
                if (app.currentRoomId !== "" && !app.eventContext.active)
                    timeline.settleAtLatest()
            }
        }
    }

    QtObject {
        id: sideSurface
        readonly property int width:
            Math.max(260, Math.min(app.settings.sidePanelWidth,
                                   root.width - 320 - 1))
        readonly property bool beside: root.width - 1 - width >= 320
        readonly property bool threadBeside: beside && !root.threadInMainView
        readonly property int infoWidth:
            Math.max(260, Math.min(app.settings.sidePanelWidth,
                                   root.width - 420))
        readonly property bool infoBeside: root.width >= 700
        readonly property bool infoTakesPane:
            root.infoOpen && root.width > 0 && !infoBeside
    }

    Item {
        id: sidePanelResizer
        objectName: "sidePanelResizeHandle"
        visible: (root.threadSurfaceOpen && sideSurface.threadBeside)
                 || (root.searchOpen && sideSurface.beside)
        Layout.fillHeight: true
        implicitWidth: 1

        Rectangle {
            anchors.fill: parent
            color: sideDrag.active ? AppTheme.accent : AppTheme.borderStrong
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        HoverHandler {
            margin: 4
            cursorShape: Qt.SplitHCursor
        }
        DragHandler {
            id: sideDrag
            margin: 4
            target: null
            yAxis.enabled: false
            property int startWidth: 0
            onActiveChanged: {
                if (active)
                    startWidth = sideSurface.width
            }
            onTranslationChanged: {
                if (!active)
                    return
                app.settings.sidePanelWidth =
                    Math.round(startWidth - translation.x)
            }
        }
    }
    ThreadPanel {
        id: threadPanel
        objectName: "threadPanel"
        visible: root.threadSurfaceOpen
        inMainView: root.threadInMainView
        Layout.fillHeight: true
        Layout.preferredWidth: sideSurface.threadBeside ? sideSurface.width
                                                        : root.width
        Layout.fillWidth: root.threadSurfaceOpen && !sideSurface.threadBeside
        onCloseRequested: {
            root.closeThreadSurface()
        }
        openImage: function(mediaKey, httpUrl) {
            imageViewer.openFor(mediaKey || "", httpUrl)
        }
        openArtwork: function(source, title) {
            imageViewer.openArtwork(source, title)
        }
        saveMedia: function(mediaKey, filename, mime, ask) {
            root.startDownload(mediaKey, filename, mime, ask, "thread")
        }
    }

    Item {
        id: infoResizer
        objectName: "roomInfoResizeHandle"
        visible: root.infoOpen && !sideSurface.infoTakesPane
        Layout.fillHeight: true
        implicitWidth: 1

        Rectangle {
            anchors.fill: parent
            color: infoDrag.active ? AppTheme.accent
                 : infoHover.hovered ? AppTheme.borderStrong : AppTheme.border
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        HoverHandler {
            id: infoHover
            margin: 4
            cursorShape: Qt.SplitHCursor
        }
        DragHandler {
            id: infoDrag
            margin: 4
            target: null
            yAxis.enabled: false
            property int startWidth: 0
            onActiveChanged: {
                if (active)
                    startWidth = app.settings.sidePanelWidth
            }
            onTranslationChanged: {
                if (!active)
                    return
                app.settings.sidePanelWidth =
                    Math.round(startWidth - translation.x)
            }
        }
    }
    RoomInfoPanel {
        id: infoPanel
        objectName: "roomInfoPanel"
        Layout.fillHeight: true
        Layout.preferredWidth: root.infoOpen
                               ? (sideSurface.infoTakesPane ? root.width
                                                            : sideSurface.infoWidth)
                               : 0
        Layout.fillWidth: sideSurface.infoTakesPane
        visible: root.infoOpen
        onCloseRequested: {
            root.infoOpen = false
            root.rememberMemberPanel()
        }
        onOpenImagesRequested: (entries, index) =>
            imageViewer.openAt(entries, index)
        onSaveMediaRequested: (mediaKey, filename) =>
            root.startDownload(mediaKey, filename, "", false)
        onJumpToEventRequested: (eventId) => {
            if (eventId !== "")
                app.pagination.jumpToEvent(eventId)
        }
        onExportRoomRequested: root.openExportRoom()
    }

    SearchPanel {
        id: searchPanel
        objectName: "roomSearchPanel"
        visible: root.searchOpen
        Layout.fillHeight: true
        Layout.preferredWidth: sideSurface.beside ? sideSurface.width
                                                  : root.width
        Layout.fillWidth: root.searchOpen && !sideSurface.beside
        historyAvailable: root.findHistoryAvailable
        serverAvailable: root.serverSearchAvailable
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

    }

    Component {
        id: spaceHomeComponent
        Rectangle {
            id: spaceHome
            objectName: "spaceHomePane"
            color: AppTheme.timelineBackground

            readonly property string spaceId:
                app.spaces ? app.spaces.activeSpaceId : ""
            property var info: ({})
            // Assigned by rebuildLobby(), never bound: its inputs come from a
            // C++ call, and a binding through a function call does not track
            // what it reads.
            property var lobbySections: []
            property var lobbySubspaceIds: []
            property var hierarchyBySpace: ({})
            property bool homeLoading: false
            property var loadingIds: ({})
            property var unjoinedChildren: []
            property string addNotice: ""
            property string pendingAddRoomId: ""
            function clearPendingAddNotice() {
                if (pendingAddRoomId === "")
                    return
                pendingAddRoomId = ""
                addNotice = ""
                addConfirmTimer.stop()
            }
            Timer {
                id: addConfirmTimer
                interval: 8000
                repeat: false
                onTriggered: spaceHome.clearPendingAddNotice()
            }
            property bool settingsOpen: false
            Connections {
                target: app.spaces
                function onSettingsDialogOpened() { spaceHome.settingsOpen = false }
            }
            readonly property bool peopleOpen: app.settings.memberPanelOpen
            readonly property var spacePeople:
                app.roomInfo ? (app.roomInfo.joinedMembers || []) : []

            InvitePeopleDialog {
                id: spaceInviteDialog
                parent: Overlay.overlay
            }
            SpaceMemberActionDialog {
                id: spaceChipMemberAction
            }

            Connections {
                target: app.discovery
                function onSpaceJoined(joinedId) {
                    if (!app.spaces)
                        return
                    var offers = spaceHome.unjoinedChildren || []
                    for (var i = 0; i < offers.length; ++i) {
                        if (offers[i].roomId === joinedId) {
                            app.spaces.activeSpaceId = joinedId
                            return
                        }
                    }
                }
            }

            function refresh() {
                info = app.spaces ? app.spaces.spaceInfo(spaceId) : {}
                if (pendingAddRoomId !== "" && app.spaces
                        && app.spaces.includesRoom(spaceId, pendingAddRoomId))
                    clearPendingAddNotice()
                lobbySubspaceIds = app.spaces
                                 ? app.spaces.lobbySubspaceIds(spaceId) : []
                if (app.discovery.supported) {
                    for (var i = 0; i < lobbySubspaceIds.length; ++i) {
                        if (app.discovery.spaceChildrenState(
                                lobbySubspaceIds[i]) === "")
                            app.discovery.refreshSpaceChildren(
                                lobbySubspaceIds[i])
                    }
                }
                refreshUnjoined()
            }
            function requestHierarchy() {
                if (spaceId === "" || !app.discovery.supported)
                    return
                app.discovery.refreshSpaceChildren(spaceId)
                for (var i = 0; i < lobbySubspaceIds.length; ++i)
                    app.discovery.refreshSpaceChildren(lobbySubspaceIds[i])
            }
            function requestHierarchyFor(roomId) {
                if (spaceId === "" || !app.discovery.supported)
                    return
                var asked = false
                for (var id in hierarchyBySpace) {
                    var rows = hierarchyBySpace[id] || []
                    for (var i = 0; i < rows.length; ++i) {
                        if (rows[i].roomId === roomId) {
                            app.discovery.refreshSpaceChildren(id)
                            asked = true
                            break
                        }
                    }
                }
                if (!asked)
                    app.discovery.refreshSpaceChildren(spaceId)
            }
            function refreshUnjoined() {
                var byspace = {}
                var loading = {}
                var out = []
                if (spaceId !== "" && app.discovery.supported) {
                    var ids = [spaceId].concat(lobbySubspaceIds)
                    for (var i = 0; i < ids.length; ++i) {
                        var rows = app.discovery.spaceChildren(ids[i])
                        byspace[ids[i]] = rows
                        if (app.discovery.spaceChildrenState(ids[i])
                                === "loading")
                            loading[ids[i]] = true
                        for (var j = 0; j < rows.length; ++j) {
                            if (rows[j].membership !== "joined")
                                out.push(rows[j])
                        }
                    }
                }
                hierarchyBySpace = byspace
                loadingIds = loading
                homeLoading = loading[spaceId] === true
                unjoinedChildren = out
                scheduleRebuild()
            }
            function scheduleRebuild() { lobbyRebuildTimer.restart() }
            Timer {
                id: lobbyRebuildTimer
                interval: 0
                repeat: false
                onTriggered: spaceHome.rebuildLobby()
            }
            Timer {
                id: unjoinedRefreshTimer
                interval: 0
                repeat: false
                onTriggered: spaceHome.refreshUnjoined()
            }
            function rebuildLobby() {
                lobbySections = app.spaces && spaceId !== ""
                    ? app.spaces.lobbySections(spaceId, hierarchyBySpace,
                                               childFilter)
                    : []
            }
            property string childFilter: ""
            property var selectedChildIds: ({})
            readonly property int selectedCount:
                Object.keys(selectedChildIds).length
            readonly property bool canManageChildren:
                app.roomInfo.roomId === spaceHome.spaceId
                && app.roomInfo.canManageSpaceChildren
            function toggleChildSelected(roomId) {
                var next = {}
                for (var k in selectedChildIds)
                    next[k] = true
                if (next[roomId])
                    delete next[roomId]
                else
                    next[roomId] = true
                selectedChildIds = next
            }
            onSpaceIdChanged: {
                addNotice = ""
                pendingAddRoomId = ""
                addConfirmTimer.stop()
                selectedChildIds = ({})
                childFilter = ""
                removeChildConfirm.close()
                removeChildConfirm.roomIds = []
                spaceLobby.closeMenus()
                refresh()
                requestHierarchy()
                if (spaceId !== "" && app.roomInfo)
                    app.roomInfo.roomId = spaceId
            }
            Component.onCompleted: {
                refresh()
                requestHierarchy()
                if (spaceId !== "" && app.roomInfo)
                    app.roomInfo.roomId = spaceId
            }
            Connections {
                target: app.discovery
                function onSpaceChildrenChanged(changedSpaceId) {
                    if (changedSpaceId === spaceHome.spaceId
                            || spaceHome.lobbySubspaceIds.indexOf(
                                   changedSpaceId) >= 0)
                        unjoinedRefreshTimer.restart()
                }
                function onRoomJoined(roomId) {
                    spaceHome.requestHierarchyFor(roomId)
                }
                function onKnockSent(roomId) {
                    spaceHome.requestHierarchyFor(roomId)
                }
            }
            Timer {
                id: spaceRefreshCoalesce
                interval: 250
                repeat: false
                onTriggered: spaceHome.refresh()
            }
            Timer {
                id: suggestRefreshCoalesce
                interval: 400
                repeat: false
                onTriggered: {
                    if (spaceHome.spaceId !== "" && app.discovery.supported)
                        app.discovery.refreshSpaceChildren(spaceHome.spaceId)
                }
            }
            Connections {
                target: app.spaces
                function onSpacesChanged() { spaceRefreshCoalesce.restart() }
                function onLobbyCollapseChanged(changedSpaceId) {
                    if (changedSpaceId === spaceHome.spaceId)
                        spaceHome.rebuildLobby()
                }
                function onChildAddFinished(spaceId, roomId, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    spaceHome.addNotice = ok
                        ? qsTr("Room added — waiting for the server to "
                               + "confirm.")
                        : qsTr("The room could not be added to this Space.")
                    spaceHome.pendingAddRoomId = ok ? roomId : ""
                    if (ok)
                        addConfirmTimer.restart()
                    else
                        addConfirmTimer.stop()
                    spaceHome.refresh()
                }
                function onChildRemoveFinished(spaceId, roomId, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    spaceHome.pendingAddRoomId = ""
                    addConfirmTimer.stop()
                    spaceHome.addNotice = ok
                        ? qsTr("Room removed from this Space. The room "
                               + "itself is untouched.")
                        : qsTr("The room could not be removed — you may "
                               + "not have permission.")
                    spaceHome.refresh()
                    suggestRefreshCoalesce.restart()
                }
                function onChildSuggestedFinished(spaceId, roomId,
                                                  suggested, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    if (!ok) {
                        spaceHome.pendingAddRoomId = ""
                        addConfirmTimer.stop()
                        spaceHome.addNotice =
                            qsTr("The suggested flag could not be changed "
                                 + "— you may not have permission.")
                    }
                    suggestRefreshCoalesce.restart()
                }
            }

            Flickable {
                id: spaceScroll
                anchors.fill: parent
                contentWidth: width
                contentHeight: spaceCol.y + spaceCol.implicitHeight
                               + AppTheme.spacing24
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

                Rectangle {
                    id: spaceBannerCard
                    objectName: "spaceHomeBanner"
                    readonly property real bannerAspect:
                        (spaceBannerImage.status === Image.Ready
                         && spaceBannerImage.implicitHeight > 0)
                        ? (spaceBannerImage.implicitWidth
                           / spaceBannerImage.implicitHeight)
                        : 0
                    readonly property bool expanded:
                        app.settings.spaceBannerExpanded
                    x: 0
                    width: spaceScroll.width
                    height: Math.round(expanded && bannerAspect > 0
                        ? Math.max(120, Math.min(420, width / bannerAspect))
                        : 190)
                    clip: true
                    visible: app.settings.spaceBannersVisible
                             && (bannerMxc.length > 0 || canEdit)
                    color: AppTheme.cardElevated

                    readonly property string bannerMxc: {
                        if (!app.banners || spaceHome.spaceId === "")
                            return ""
                        var _dep = app.banners.revision
                        return app.banners.roomBannerFor(spaceHome.spaceId)
                    }
                    readonly property bool canEdit: {
                        if (!app.banners || spaceHome.spaceId === "")
                            return false
                        var _dep = app.banners.revision
                        return app.banners.canSetRoomBanner(
                            spaceHome.spaceId)
                    }

                    onVisibleChanged: if (visible) spaceBannerCard.ask()
                    Component.onCompleted: spaceBannerCard.ask()
                    function ask() {
                        if (app.banners && spaceHome.spaceId !== "")
                            app.banners.requestRoom(spaceHome.spaceId)
                    }
                    Connections {
                        target: spaceHome
                        function onSpaceIdChanged() { spaceBannerCard.ask() }
                    }

                    Rectangle {
                        anchors.fill: parent
                        visible: !spaceBannerImage.visible
                        gradient: Gradient {
                            GradientStop { position: 0.0
                                color: AppTheme.cardElevated }
                            GradientStop { position: 1.0
                                color: AppTheme.hover }
                        }
                    }

                    Image {
                        id: spaceBannerImage
                        anchors.fill: parent
                        fillMode: spaceBannerCard.expanded
                                  ? Image.PreserveAspectFit
                                  : Image.PreserveAspectCrop
                        sourceSize.width: 1600
                        asynchronous: true
                        visible: status === Image.Ready
                        opacity: spaceBannerMotion.shown ? 0 : 1
                        readonly property string mxc:
                            spaceBannerCard.bannerMxc
                        // A counter, never an assignment to `source`: assigning
                        // a bound property destroys its binding (see
                        // MemberProfilePopover).
                        property int resolveTick: 0
                        source: {
                            var _tick = resolveTick
                            return mxc.length > 0 && app.mediaBridge.supported
                                   ? app.mediaBridge.wideImageSource(mxc) : ""
                        }
                        Connections {
                            target: app.mediaBridge
                            enabled: spaceBannerImage.mxc.length > 0
                            function onMediaCached(key) {
                                if (key.endsWith(":" + spaceBannerImage.mxc)
                                    && spaceBannerImage.source.toString()
                                           .length === 0)
                                    spaceBannerImage.resolveTick++
                            }
                        }
                    }
                    BannerMotion {
                        id: spaceBannerMotion
                        objectName: "spaceBannerMotion"
                        anchors.fill: parent
                        mxc: spaceBannerImage.mxc
                        stillReady: spaceBannerImage.status === Image.Ready
                        fillMode: spaceBannerImage.fillMode
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: spaceBannerImage.visible
                        gradient: Gradient {
                            GradientStop { position: 0.0
                                color: "transparent" }
                            GradientStop { position: 1.0
                                color: AppTheme.overlayScrim }
                        }
                        opacity: 0.55
                    }

                    NativeFileDialog {
                        id: spaceBannerDialog
                        purpose: "image"
                        title: qsTr("Choose a banner image")
                        currentFolder: app.defaultFileDialogFolder()
                        fileMode: "open"
                        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.svg)") ]
                        onAccepted: spaceBannerCrop.openFor(selectedFile)
                    }
                    ImageCropDialog {
                        id: spaceBannerCrop
                        role: "banner"
                        onCropped: function (file) {
                            app.banners.setRoomBanner(
                                spaceHome.spaceId, file.toString())
                        }
                    }

                    Rectangle {
                        objectName: "spaceBannerControls"
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: AppTheme.spacing8
                        radius: height / 2
                        color: AppTheme.scrimSurface
                        border.width: 1
                        border.color: AppTheme.overlayScrim
                        readonly property int pad: AppTheme.spacing4
                        implicitWidth: bannerControlRow.implicitWidth + pad * 2
                        implicitHeight: bannerControlRow.implicitHeight + pad * 2
                        visible: spaceBannerCard.bannerMxc.length > 0
                                 || spaceBannerCard.canEdit

                        Row {
                            id: bannerControlRow
                            x: parent.pad
                            y: parent.pad
                            spacing: 2

                            IconButton {
                                id: bannerExpandBtn
                                objectName: "spaceBannerExpandButton"
                                visible: spaceBannerCard.bannerMxc.length > 0
                                implicitWidth: 28; implicitHeight: 28
                                radius: 14
                                iconColorOverride: AppTheme.scrimInk
                                iconName: spaceBannerCard.expanded
                                          ? "close_fullscreen" : "open_in_full"
                                iconSize: 17
                                Accessible.name: spaceBannerCard.expanded
                                    ? qsTr("Crop the banner")
                                    : qsTr("Show the whole banner")
                                ToolTip.text: Accessible.name
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: app.settings.spaceBannerExpanded =
                                    !app.settings.spaceBannerExpanded
                            }
                            IconButton {
                                id: bannerHideBtn
                                objectName: "spaceBannerHideButton"
                                visible: spaceBannerCard.bannerMxc.length > 0
                                implicitWidth: 28; implicitHeight: 28
                                radius: 14
                                iconColorOverride: AppTheme.scrimInk
                                iconName: "visibility_off"
                                iconSize: 17
                                Accessible.name: qsTr("Hide space banners")
                                ToolTip.text: Accessible.name
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: app.settings.spaceBannersVisible = false
                            }
                            IconButton {
                                id: bannerChangeBtn
                                objectName: "spaceBannerChangeButton"
                                visible: spaceBannerCard.canEdit
                                enabled: !app.banners.busy
                                implicitWidth: 28; implicitHeight: 28
                                radius: 14
                                iconColorOverride: AppTheme.scrimInk
                                iconName: "image"
                                iconSize: 17
                                Accessible.name:
                                    spaceBannerCard.bannerMxc.length > 0
                                    ? qsTr("Change banner") : qsTr("Add a banner")
                                ToolTip.text: Accessible.name
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: spaceBannerDialog.open()
                            }
                            IconButton {
                                id: bannerRemoveBtn
                                objectName: "spaceBannerRemoveButton"
                                visible: spaceBannerCard.canEdit
                                         && spaceBannerCard.bannerMxc.length > 0
                                enabled: !app.banners.busy
                                implicitWidth: 28; implicitHeight: 28
                                radius: 14
                                iconColorOverride: AppTheme.dangerInk
                                iconName: "delete"
                                iconSize: 17
                                Accessible.name: qsTr("Remove banner")
                                ToolTip.text: Accessible.name
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: app.banners.clearRoomBanner(
                                    spaceHome.spaceId)
                            }
                        }
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: AppTheme.spacing8
                        width: parent.width - AppTheme.spacing16
                        visible: text.length > 0
                        text: {
                            if (!app.banners
                                    || app.banners.lastError.length === 0)
                                return ""
                            return qsTr("The banner could not be saved (%1).")
                                .arg(app.banners.lastError)
                        }
                        color: AppTheme.danger
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    id: spaceCol
                    width: Math.min(880,
                                    spaceScroll.width - AppTheme.spacing24 * 2)
                    x: Math.round((spaceScroll.width - width) / 2)
                    y: (spaceBannerCard.visible
                        ? spaceBannerCard.height + AppTheme.spacing16
                        : AppTheme.spacing24)
                    spacing: AppTheme.spacing16

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        Avatar {
                            objectName: "spaceHomeAvatar"
                            size: 56
                            name: spaceHome.info.name || ""
                            mxc: spaceHome.info.avatarUrl || ""
                            colorKey: spaceHome.spaceId
                            prominent: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                objectName: "spaceHomeName"
                                text: spaceHome.info.name || qsTr("Space")
                                textFormat: Text.PlainText
                                color: AppTheme.text
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textDisplay)
                                font.weight: AppTheme.weightDisplay
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            TopicText {
                                objectName: "spaceHomeTopic"
                                visible: (spaceHome.info.topic || "").length > 0
                                plainText: spaceHome.info.topic || ""
                                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var c = spaceHome.info.childCount || 0
                                    var u = spaceHome.info.unreadTotal || 0
                                    var line = qsTr("%n joined room(s)",
                                                    "joined rooms inside a Space", c)
                                    if (u > 0)
                                        line = qsTr("%1 • %2 unread")
                                            .arg(line).arg(u)
                                    return line
                                }
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingS
                        AppButton {
                            objectName: "spaceCreateRoomButton"
                            kind: "primary"
                            text: qsTr("Create room here")
                            visible: app.conversations
                                     && app.conversations.supported
                                     && spaceHome.canManageChildren
                            onClicked: root.newConversationRequested(
                                           "room",
                                           { addToSpace:
                                                 spaceHome.canManageChildren })
                        }
                        AppButton {
                            objectName: "spaceInviteButton"
                            visible: app.roomInfo
                                     && app.roomInfo.roomId === spaceHome.spaceId
                                     && app.roomInfo.canInvite
                            text: qsTr("Invite")
                            onClicked: spaceInviteDialog.openFor(
                                           spaceHome.spaceId)
                        }
                        AppButton {
                            objectName: "spaceAddRoomButton"
                            text: qsTr("Add existing room")
                            visible: spaceHome.canManageChildren
                            onClicked: {
                                addRoomPopup.query = ""
                                addRoomPopup.refresh()
                                addRoomPopup.open()
                            }
                        }
                        AppButton {
                            objectName: "spacePeopleButton"
                            visible: app.roomInfo
                                     && app.roomInfo.roomId === spaceHome.spaceId
                            text: spaceHome.peopleOpen
                                  ? qsTr("Hide people")
                                  : qsTr("People (%1)").arg(
                                        spaceHome.spacePeople.length)
                            onClicked: {
                                const opening = !spaceHome.peopleOpen
                                app.settings.memberPanelOpen = opening
                                if (opening)
                                    spacePeopleCard.reveal()
                                else
                                    spacePeopleCard.revealPending = false
                            }
                        }
                        AppButton {
                            objectName: "spaceSettingsButton"
                            text: spaceHome.settingsOpen
                                  ? qsTr("Hide settings") : qsTr("Space settings")
                            onClicked:
                                spaceHome.settingsOpen = !spaceHome.settingsOpen
                        }
                        AppButton {
                            objectName: "spaceBannerShowButton"
                            visible: !app.settings.spaceBannersVisible
                                     && spaceBannerCard.bannerMxc.length > 0
                            iconName: "visibility"
                            text: qsTr("Show banner")
                            onClicked: app.settings.spaceBannersVisible = true
                        }
                    }

                    Label {
                        objectName: "spaceHomeNotice"
                        visible: spaceHome.addNotice.length > 0
                        text: spaceHome.addNotice
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    Rectangle {
                        visible: spaceHome.settingsOpen
                        Layout.fillWidth: true
                        radius: AppTheme.radiusMd
                        color: AppTheme.cardElevated
                        border.color: AppTheme.border
                        border.width: 1
                        implicitHeight: settingsCol.implicitHeight
                                        + AppTheme.spacing16 * 2
                        NativeFileDialog {
                            id: spaceAvatarDialog
                            purpose: "image"
                            title: qsTr("Choose Space avatar")
                            currentFolder: app.defaultFileDialogFolder()
                            fileMode: "open"
                            nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)") ]
                            onAccepted: spaceAvatarCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceAvatarCrop
                            role: "avatar"
                            onCropped: function (file) {
                                app.roomInfo.setRoomAvatar(file)
                            }
                        }
                        ColumnLayout {
                            id: settingsCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing16
                            spacing: AppTheme.spacingS
                            Label {
                                text: qsTr("Avatar")
                                color: AppTheme.textSecondary
                                font.family: AppTheme.menuSectionFont
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.menuSectionSize)
                                font.weight: AppTheme.menuSectionWeight
                                font.letterSpacing: AppTheme.menuSectionTracking
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                AppButton {
                                    objectName: "spaceChangeAvatarButton"
                                    text: qsTr("Change avatar…")
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditAvatar
                                             && !app.roomInfo.editPending
                                    onClicked: spaceAvatarDialog.open()
                                    Accessible.name: qsTr("Change the Space avatar")
                                    ToolTip.visible: hovered && !enabled
                                                     && !!app.roomInfo
                                                     && !app.roomInfo.canEditAvatar
                                    ToolTip.delay: 500
                                    ToolTip.text: qsTr("Only Space admins can change this.")
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
                                color: AppTheme.textSecondary
                                font.family: AppTheme.menuSectionFont
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.menuSectionSize)
                                font.weight: AppTheme.menuSectionWeight
                                font.letterSpacing: AppTheme.menuSectionTracking
                            }
                            SpaceTextEditRow {
                                id: spaceNameRow
                                Layout.fillWidth: true
                                targetId: spaceHome.spaceId
                                authoritative: spaceHome.info.name || ""
                                targetCurrent: !!app.roomInfo
                                               && app.roomInfo.roomId === spaceHome.spaceId
                                canEdit: !!app.roomInfo
                                         && app.roomInfo.canEditName
                                pending: !!app.roomInfo
                                         && app.roomInfo.editPending
                                requireNonEmpty: true
                                buttonText: qsTr("Rename")
                                fieldObjectName: "spaceNameEditField"
                                buttonObjectName: "spaceRenameButton"
                                accessibleName: qsTr("Space name")
                                onSubmitted: (value) => {
                                    if (app.roomInfo
                                            && app.roomInfo.roomId === spaceHome.spaceId
                                            && !app.roomInfo.editPending)
                                        app.roomInfo.setRoomName(value)
                                }
                            }
                            Label {
                                text: qsTr("Topic")
                                color: AppTheme.textSecondary
                                font.family: AppTheme.menuSectionFont
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.menuSectionSize)
                                font.weight: AppTheme.menuSectionWeight
                                font.letterSpacing: AppTheme.menuSectionTracking
                            }
                            SpaceTextEditRow {
                                id: spaceTopicRow
                                Layout.fillWidth: true
                                targetId: spaceHome.spaceId
                                authoritative: spaceHome.info.topic || ""
                                targetCurrent: !!app.roomInfo
                                               && app.roomInfo.roomId === spaceHome.spaceId
                                canEdit: !!app.roomInfo
                                         && app.roomInfo.canEditTopic
                                pending: !!app.roomInfo
                                         && app.roomInfo.editPending
                                buttonText: qsTr("Save")
                                fieldObjectName: "spaceTopicEditField"
                                buttonObjectName: "spaceTopicSaveButton"
                                accessibleName: qsTr("Space topic")
                                onSubmitted: (value) => {
                                    if (app.roomInfo
                                            && app.roomInfo.roomId === spaceHome.spaceId
                                            && !app.roomInfo.editPending)
                                        app.roomInfo.setRoomTopic(value)
                                }
                            }
                            Label {
                                visible: app.roomInfo
                                         && app.roomInfo.editError.length > 0
                                text: app.roomInfo ? app.roomInfo.editError : ""
                                color: AppTheme.danger
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                                    font.family: AppTheme.uiFont
                                    font.pixelSize:
                                        AppTheme.scaled(AppTheme.textMeta)
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }

                    SpaceLobby {
                        id: spaceLobby
                        Layout.fillWidth: true
                        sections: spaceHome.lobbySections
                        canManage: spaceHome.canManageChildren
                        selectedIds: spaceHome.selectedChildIds
                        filterText: spaceHome.childFilter
                        busy: app.discovery.busy
                        errorMessage: app.discovery.errorMessage
                        homeLoading: spaceHome.homeLoading
                        loadingIds: spaceHome.loadingIds
                        onFilterEdited: (text) => {
                            spaceHome.childFilter = text
                            spaceHome.scheduleRebuild()
                        }
                        onOpenRoomRequested: (roomId) => {
                            app.openRoom(roomId)
                            app.requestComposerFocus(roomId)
                        }
                        onOpenSpaceRequested: (roomId) =>
                            app.spaces.activeSpaceId = roomId
                        onJoinRequested: (roomId, via, isSpace) =>
                            app.discovery.join(roomId, via || [], isSpace)
                        onKnockRequested: (roomId, via) =>
                            app.discovery.knock(roomId, via || [], "")
                        onSelectionToggled: (roomId) =>
                            spaceHome.toggleChildSelected(roomId)
                        onCollapseToggled: (sectionId, collapsed) =>
                            app.spaces.setLobbySectionCollapsed(
                                spaceHome.spaceId, sectionId, collapsed)
                        onRemoveRequested: (roomIds) => {
                            removeChildConfirm.roomIds = roomIds
                            removeChildConfirm.open()
                        }
                        onSuggestRequested: (roomIds, suggested) => {
                            for (var i = 0; i < roomIds.length; ++i)
                                app.spaces.setSpaceChildSuggested(
                                    spaceHome.spaceId, roomIds[i], suggested)
                            spaceHome.selectedChildIds = ({})
                        }
                    }

                    Rectangle {
                        id: spacePeopleCard
                        objectName: "spacePeopleCard"
                        property bool revealPending: false
                        function reveal() {
                            revealPending = true
                            revealWindow.restart()
                            Qt.callLater(tryReveal)
                        }
                        function tryReveal() {
                            if (!revealPending || !visible || height <= 0)
                                return
                            if (y < spaceLobby.y + spaceLobby.height)
                                return
                            const top = spaceCol.y + y
                            const want = Math.min(height, spaceScroll.height / 2)
                            const viewBottom = spaceScroll.contentY
                                               + spaceScroll.height
                            if (top + want <= viewBottom) {
                                revealPending = false
                                return
                            }
                            const maxY = Math.max(0, spaceScroll.contentHeight
                                                     - spaceScroll.height)
                            const target = Math.max(spaceScroll.contentY,
                                                    top + want
                                                    - spaceScroll.height
                                                    + AppTheme.spacing24)
                            spaceScroll.contentY = Math.min(maxY, target)
                            if (target <= maxY)
                                revealPending = false
                        }
                        onYChanged: tryReveal()
                        onHeightChanged: tryReveal()
                        Connections {
                            target: spaceScroll
                            function onContentHeightChanged() {
                                spacePeopleCard.tryReveal()
                            }
                        }
                        Timer {
                            id: revealWindow
                            interval: 600
                            onTriggered: spacePeopleCard.revealPending = false
                        }
                        visible: spaceHome.peopleOpen
                                 && app.roomInfo
                                 && app.roomInfo.roomId === spaceHome.spaceId
                        Layout.fillWidth: true
                        radius: AppTheme.radiusMd
                        color: AppTheme.cardElevated
                        border.color: AppTheme.border
                        border.width: 1
                        implicitHeight: spacePeopleCol.implicitHeight
                                        + AppTheme.spacing16 * 2

                        ColumnLayout {
                            id: spacePeopleCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: AppTheme.spacing16
                            spacing: AppTheme.spacing8

                            Label {
                                text: qsTr("People in this Space")
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.textBody
                                font.weight: AppTheme.weightStrong
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("Members of the Space itself. Its rooms each have their own members.")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }

                            Flow {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Repeater {
                                    model: spaceHome.spacePeople.slice(0, 60)
                                    delegate: SpaceMemberChip {
                                        spaceId: spaceHome.spaceId
                                        onProfileRequested: (member) =>
                                            senderProfilePopover.openFor(member, parent)
                                        onModerationRequested: (member, action) =>
                                            spaceChipMemberAction.openFor(
                                                spaceHome.spaceId,
                                                spaceHome.info.name || spaceHome.spaceId,
                                                member.userId,
                                                member.displayName || member.userId,
                                                action)
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                visible: spaceHome.spacePeople.length > 60
                                text: qsTr("Showing the first 60 of %1.")
                                      .arg(spaceHome.spacePeople.length)
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                    }
                }
            }

            Popup {
                id: removeChildConfirm
                property var roomIds: []
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                Overlay.modal: Rectangle { color: AppTheme.modalScrim }
                background: Rectangle {
                    color: AppTheme.stormPanel
                    radius: AppTheme.radiusLg
                    border.color: AppTheme.stormBorder
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Remove %n room(s) from this Space?", "",
                                   removeChildConfirm.roomIds.length)
                        color: AppTheme.stormText
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: qsTr("The rooms keep existing and you stay "
                                   + "in them — they just leave this "
                                   + "Space's list.")
                        color: AppTheme.stormTextSecondary
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Item { Layout.fillWidth: true }
                        AppButton {
                            storm: true
                            text: qsTr("Cancel")
                            onClicked: removeChildConfirm.close()
                        }
                        AppButton {
                            objectName: "spaceChildRemoveConfirmButton"
                            storm: true
                            kind: "danger"
                            text: qsTr("Remove")
                            onClicked: {
                                var ids = removeChildConfirm.roomIds
                                for (var i = 0; i < ids.length; ++i)
                                    app.spaces.removeRoomFromSpace(
                                        spaceHome.spaceId, ids[i])
                                spaceHome.selectedChildIds = ({})
                                removeChildConfirm.close()
                            }
                        }
                    }
                }
            }

            Popup {
                id: leaveSpaceConfirm
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                Overlay.modal: Rectangle { color: AppTheme.modalScrim }
                background: Rectangle {
                    color: AppTheme.stormPanel
                    radius: AppTheme.radiusLg
                    border.color: AppTheme.stormBorder
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Leave %1?")
                            .arg(spaceHome.info.name || qsTr("this Space"))
                        textFormat: Text.PlainText
                        color: AppTheme.stormText
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: qsTr("The rooms inside stay untouched.")
                        color: AppTheme.stormTextSecondary
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    }
                    Label {
                        objectName: "spaceLeaveLastAdminNote"
                        Layout.fillWidth: true
                        readonly property bool lastAdmin:
                            leaveSpaceConfirm.visible
                            && AdminCheck.soleAdminOf(app.roomInfo,
                                                      spaceHome.spaceId)
                        visible: lastAdmin
                        textFormat: Text.PlainText
                        text: qsTr("You are the only administrator. If you "
                                   + "leave, nobody will be able to change "
                                   + "this Space's settings or manage its "
                                   + "rooms and members. Make someone else "
                                   + "an administrator first.")
                        wrapMode: Text.WordWrap
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        font.weight: AppTheme.weightStrong
                        color: AppTheme.stormText
                    }
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Item { Layout.fillWidth: true }
                        AppButton {
                            storm: true
                            text: qsTr("Cancel")
                            onClicked: leaveSpaceConfirm.close()
                        }
                        AppButton {
                            objectName: "spaceLeaveConfirmButton"
                            storm: true
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
                Overlay.modal: Rectangle { color: AppTheme.modalScrim }
                background: Rectangle {
                    color: AppTheme.stormPanel
                    radius: AppTheme.radiusLg
                    border.color: AppTheme.stormBorder
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Add a room to %1")
                            .arg(spaceHome.info.name || qsTr("this Space"))
                        textFormat: Text.PlainText
                        color: AppTheme.stormText
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                        font.weight: AppTheme.weightBold
                    }
                    AppTextField {
                        objectName: "spaceAddRoomSearch"
                        Layout.fillWidth: true
                        searchIcon: true
                        clearButton: true
                        storm: true
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
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(280, contentHeight)
                        clip: true
                        model: addRoomPopup.results
                        spacing: 2
                        ScrollBar.vertical: AppScrollBar {
                            policy: ScrollBar.AsNeeded
                        }
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 40
                            radius: AppTheme.radiusSm
                            color: addHover.hovered
                                   ? AppTheme.stormSelection : "transparent"
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
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name || qsTr("Room")
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormText
                                    font.family: AppTheme.menuFont
                                    font.pixelSize: AppTheme.scaled(
                                        AppTheme.textBody)
                                    elide: Label.ElideRight
                                }
                                Label {
                                    visible: modelData.alreadyChild === true
                                    text: qsTr("Already added")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.menuFont
                                    font.pixelSize: AppTheme.scaled(
                                        AppTheme.textMeta)
                                }
                                AppButton {
                                    visible: modelData.alreadyChild !== true
                                    storm: true
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
                            storm: true
                            text: qsTr("Close")
                            onClicked: addRoomPopup.close()
                        }
                    }
                }
            }
        }
    }

    readonly property bool voiceDockShown: app.voicePlayback.active
        && app.settings.voicePlayerDocked && roomColumn.visible
    function focusVoiceDock() {
        voiceDock.focusPlayer()
    }
    Item {
        id: voiceCardProxy
        visible: false
        property Item card: null
        function refresh() {
            var w = root.Window.window
            if (!card && w && w.contentItem)
                card = root.findVoiceCard(w.contentItem, 0)
            if (!card || !card.visible || card.opacity <= 0) {
                visible = false
                return
            }
            var p = card.mapToItem(root, 0, 0)
            x = p.x; y = p.y; width = card.width; height = card.height
            visible = true
        }
        Connections {
            target: voiceCardProxy.card
            enabled: voiceCardProxy.card !== null
            ignoreUnknownSignals: true
            function onXChanged() { voiceCardProxy.refresh() }
            function onYChanged() { voiceCardProxy.refresh() }
            function onWidthChanged() { voiceCardProxy.refresh() }
            function onHeightChanged() { voiceCardProxy.refresh() }
            function onVisibleChanged() { voiceCardProxy.refresh() }
        }
        Connections {
            target: app.voicePlayback
            function onCurrentChanged() { voiceCardProxy.refresh() }
        }
        Connections {
            target: root
            function onXChanged() { voiceCardProxy.refresh() }
            function onYChanged() { voiceCardProxy.refresh() }
            function onWidthChanged() { voiceCardProxy.refresh() }
            function onHeightChanged() { voiceCardProxy.refresh() }
        }
    }
    function findVoiceCard(item, depth) {
        if (!item || depth > 3)
            return null
        if (item.objectName === "voiceMiniPlayer")
            return item
        for (var i = 0; i < item.children.length; ++i) {
            var kid = item.children[i]
            if (kid.objectName === "voiceMiniPlayerHost"
                    || kid.objectName === "voiceMiniPlayer") {
                var found = root.findVoiceCard(kid, depth + 1)
                if (found)
                    return found
            }
        }
        return null
    }
    PaneDownloadsCard {
        id: downloadsCard
        z: 299
        safeArea: root.chatSafeArea
        avoid: voiceCardProxy
    }
    readonly property rect chatSafeArea: {
        if (!roomColumn.visible)
            return Qt.rect(0, AppTheme.headerBandHeight, root.width,
                           Math.max(0, root.height
                                       - AppTheme.headerBandHeight - 96))
        var top = roomColumn.y + roomHeaderBand.height + 1
            + (timelineCallHeaderBar.visible
               ? timelineCallHeaderBar.height : 0)
            + (legacyCallVideoHost.visible ? legacyCallVideoHost.height : 0)
            + (callStageHost.visible ? callStageHost.height : 0)
            + (voiceDock.visible ? voiceDock.height : 0)
        var bottom = roomColumn.y + timelineArea.y + timelineArea.height
        if (saveResult.text.length > 0)
            bottom -= saveResult.implicitHeight + AppTheme.spacing8
        if (jumpToLatestButton.visible)
            bottom -= jumpToLatestButton.height + AppTheme.spacingM + 8
        return Qt.rect(roomColumn.x, top, roomColumn.width,
                       Math.max(0, bottom - top))
    }

    DropArea {
        id: chatDropArea
        objectName: "chatDropArea"
        // Never cover a side surface: ThreadPanel has its own DropArea, and
        // thread attachments must use the thread send path; a drop on search
        // or Room information is not meant for the composer either. Stops at
        // the open panel's divider (the same sideSurface widths the panels are
        // laid out with); zero width when a surface takes the whole pane.
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.threadSurfaceOpen
               ? (sideSurface.threadBeside ? root.width - sideSurface.width - 1
                                           : 0)
               : root.searchOpen
               ? (sideSurface.beside ? root.width - sideSurface.width - 1 : 0)
               : root.infoOpen
               ? (sideSurface.infoBeside
                  ? root.width - sideSurface.infoWidth - 1 : 0)
               : root.width
        z: 400
        enabled: app.composer.attachmentsSupported
                 && app.currentRoomId.length > 0
        keys: ["text/uri-list", "application/vnd.portal.filetransfer",
               "application/vnd.portal.files"]
        function portalKey(drop) {
            var formats = ["application/vnd.portal.filetransfer",
                           "application/vnd.portal.files"]
            for (var i = 0; i < formats.length; ++i) {
                if (drop.formats.indexOf(formats[i]) >= 0)
                    return drop.getDataAsString(formats[i])
            }
            return ""
        }
        onDropped: (drop) => {
            var key = portalKey(drop)
            if (!drop.hasUrls && key === "") return
            app.composer.addDroppedAttachments(drop.hasUrls ? drop.urls : [], key)
            messageComposer.focusStagedAttachmentSend()
            drop.accept(Qt.CopyAction)
        }
    }
    Rectangle {
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
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                font.weight: AppTheme.weightStrong
            }
        }
    }
}
