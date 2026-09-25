import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    // The attachment name is sender-chosen. The bridge reduces it to a bare
    // leaf (no separators, leading dot or reserved device name) and it is
    // percent-encoded so '#', '?' and '%' are not read as URL syntax. An empty
    // result lets the dialog pick its own name.
    function suggestedSaveUrl(filename) {
        var leaf = app.mediaBridge.suggestedSaveName(filename || "")
        if (!leaf || leaf.length === 0)
            leaf = "download"
        return "file:///" + encodeURIComponent(leaf)
    }
    color: AppTheme.background

    // Routes Home's create actions to the room list's shared new-conversation
    // dialog. options: {addToSpace: bool} preselects placement in the active
    // Space.
    signal newConversationRequested(string mode, var options)

    property var currentRoom: ({})
    property bool infoOpen: false
    property bool searchOpen: false
    // The thread side surface is open when a thread panel or the room's Threads
    // list is showing.
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen
    /// A live MatrixRTC call in this room owns the column. One condition, read
    /// by the stage's Loader and by everything the stage replaces, so the two
    /// cannot disagree and split the column between them.
    ///
    /// Excludes picture-in-picture: SfuVideoRouter holds one sink per track and
    /// the last attach owns it, so a built stage would go black behind the PiP
    /// and stay black after it closes. Unloading the stage releases its sinks.
    readonly property bool callStageOwnsColumn:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId
        && !(app.groupCall.stageState
             && app.groupCall.stageState.pictureInPicture)

    // Call panel height: 40% of the column for voice, 70% once anything sends
    // video, clamped to [220 px, 75%]. A drag stores the user's height for the
    // session; video appearing still nudges it up once (see
    // onCallPanelHasVideoChanged). Collapsed, the panel is a one-line strip.
    property bool callPanelCollapsed: false
    /// < 0: no user preference, follow the automatic height.
    property real callPanelUserHeight: -1
    /// Drag origin: a DragHandler reports a translation, not a position.
    property real callPanelDragBase: 0
    /// Collapsed strip height: the compact control row plus the panel margins.
    /// Anything smaller clips the controls.
    readonly property real callPanelCollapsedHeight: 64
    /// Read off the stage, which already owns this derivation.
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

    /// Minimum height for a panel showing video, asked of the stage (it knows
    /// its header, dock and margins). Falls back to a flat number while the
    /// stage is not loaded.
    readonly property real callPanelVideoFloor:
        callStageHost.active && callStageHost.item
        ? callStageHost.item.minimumUsefulHeight : 220

    function clampCallPanelHeight(value) {
        // The floor yields to a small column so the timeline never disappears.
        // A panel showing video asks for more (0.6 rather than 0.45 of the
        // pane) so a share is not squeezed to a sliver.
        var floor = root.callPanelHasVideo
            ? Math.min(root.callPanelVideoFloor,
                       Math.max(64, root.height * 0.6))
            : Math.min(220, Math.max(64, root.height * 0.45))
        var ceiling = Math.max(floor, root.height * 0.75)
        return Math.max(floor, Math.min(value, ceiling))
    }

    // One-shot grow when a share or camera arrives; afterwards the user's own
    // height wins again.
    onCallPanelHasVideoChanged: {
        if (!root.callPanelHasVideo || root.callPanelUserHeight < 0)
            return
        var target = clampCallPanelHeight(root.height * 0.70)
        if (root.callPanelUserHeight < target)
            root.callPanelUserHeight = target
    }

    // Collapse is per call: the next call must not start collapsed.
    onCallStageOwnsColumnChanged: {
        if (!root.callStageOwnsColumn)
            root.callPanelCollapsed = false
    }
    // Exactly one right-side surface is open: the thread surface wins, then the
    // info/member panel. All open/close paths go through the two underlying
    // states and their exclusivity handlers.
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

    // Member/info panel and thread panel are mutually exclusive. The exclusion
    // lives on the state properties so every open path goes through it.
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

    // Header toggle for the thread surface (list or open thread).
    function toggleThreadSurface() {
        if (threadSurfaceOpen) {
            if (app.thread.active)
                app.thread.close()
            app.thread.closeList()
        } else {
            app.thread.openList(app.currentRoomId)
        }
    }

    // Opens the side panel on the Pinned section (mirrors toggleMemberPanel).
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

    // Opens the side panel on the People section.
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
        // Full-view Settings hides the chat shell. The info panel and its
        // section are remembered and restored on return; the find bar is
        // closed.
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
            // A find session belongs to the room it was opened in.
            if (root.findOpen) {
                root.findOpen = false
                root.findHistoryMode = false
                app.messageSearch.query = ""
                findField.text = ""
            }
            // Wheel motion and middle-click autoscroll must not continue into
            // the new room.
            timeline.cancelWheelMotion()
            root.stopAutoscroll()
            timeline.pinnedActionsKey = ""
            timeline.viewAnchorId = ""
            timeline.viewAnchorOffset = 0
            timeline.viewAnchorLastY = 0
            // A room switch closes the right-side panel.
            root.infoOpen = false
            root.searchOpen = false
        }
    }

    // Find in the loaded timeline, plus an explicit History mode (see below).
    // The two modes never mix results.
    property bool findOpen: false
    // History mode searches the local index by default, which covers decrypted
    // text and so works in encrypted rooms. Server search stays available where
    // the server can read the room, because it covers history this client has
    // never seen; the source is a visible choice, not a silent fallback.
    property bool findHistoryMode: false
    // Server search needs an affirmatively unencrypted room; an encryption
    // state that has not synced yet does not qualify.
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
        app.timeline.beginSearch(findField.text)
        findField.forceActiveFocus()
        findField.selectAll()
    }
    function closeFind() {
        root.findOpen = false
        root.findHistoryMode = false
        app.messageSearch.query = ""
        app.timeline.endSearch()
        // The field is about to become invisible; hand focus back to the
        // timeline rather than leaving the focus scope empty.
        timeline.forceActiveFocus()
    }
    function scrollToSearchMatch() {
        var eventId = app.timeline.searchCurrentEventId
        if (eventId === "") return
        // The match may be an older row still being paced out; release the
        // backlog first or the jump resolves to nothing.
        timeline.releasePendingRows()
        var row = timeline.viewRowForStableId(eventId)
        if (row < 0) return
        timeline.cancelWheelMotion()
        root.stopAutoscroll()
        timeline.stickToBottom = false
        timeline.positionViewAtViewRow(row, true)
    }
    Shortcut {
        // Spelled explicitly rather than StandardKey.Find so the binding can be
        // shown and rebound.
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.find")]
        }
        enabled: app.currentRoomId !== ""
        onActivated: root.openFind()
    }
    // Room info panel sections. These call the same functions as the header
    // buttons.
    //
    // Gated on app.currentScreen === 1: MainScreen stays loaded under Settings
    // and Shortcuts match by window, not visibility. Ctrl+U also underlines in
    // the composer (EditorContext), which takes it while focused; that is a
    // shadow, not an ambiguity.
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
            if (root.findOpen) root.scrollToSearchMatch()
        }
    }

    // Escape closes the room info panel first, then any pinned toolbar.
    Shortcut {
        sequence: "Escape"
        // Two enabled Shortcuts on one sequence are ambiguous and Qt fires
        // neither, so exclude the owners explicitly: an active autoscroll and a
        // forward selection have their own Escape. Also gated to this screen,
        // since Settings keeps MainScreen loaded and Shortcuts match by window.
        enabled: app.currentScreen === 1
                 && !timeline.emojiPickerOpen && !middleClickScroller.active
                 && !app.forward.selecting
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
    // Read state on focus change is handled by the timeline's maybeMarkRead().
    Component.onCompleted: refreshCurrentRoom()

    ImageViewerOverlay {
        id: imageViewer
        onOpened: timeline.claimTransientInteraction("viewer")
        onClosed: timeline.releaseTransientInteraction("viewer")
    }

    // One reaction picker, one sender-profile popover and one read-receipt list
    // for the whole timeline; rows pass plain data, never object references.
    // The target event id is captured at open; a room or account switch closes
    // them. The bridge delivers the newest 16 readers with the true total, so
    // the list ends with "+N more" rather than inventing names.
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
        // Open upward: the chips sit at a message's bottom edge.
        preferAbove: true
        onOpened: timeline.claimTransientInteraction("readers")
        onClosed: timeline.releaseTransientInteraction("readers")
        // Content-sized, capped at half the window; past the cap the list
        // scrolls.
        width: Math.min(280, maxWidth)
        height: {
            var want = (contentItem ? contentItem.implicitHeight : 0)
                       + topPadding + bottomPadding
            var cap = Math.min(maxHeight,
                               overlayItem ? overlayItem.height * 0.5 : 400)
            return Math.max(Math.min(want, cap), 96)
        }
        // Shared floating-popover surface; otherwise the Basic style draws an
        // unthemed box.
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.menuRadius + 6
        }
        // Today: time; this week: weekday + time; older: date + time. 0 means
        // the receipt had no timestamp, so render nothing.
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
                return Qt.formatDateTime(d, "ddd hh:mm")
            return Qt.formatDateTime(d, "MMM d, hh:mm")
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
                // A ListView reports 0 by default; the card sizes from this.
                implicitHeight: contentHeight
                clip: true
                spacing: 2
                model: receiptListPopover.readers
                // Themed bar. The stock Basic ScrollBar paints from the OS
                // palette and does not follow the Lightning theme.
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Item {
                    id: readerDelegate
                    required property var modelData
                    width: ListView.view.width
                    height: readerRow.implicitHeight + AppTheme.spacing8
                    Rectangle {
                        anchors.fill: parent
                        radius: AppTheme.radiusMd
                        color: AppTheme.stormSelection
                        visible: readerHover.hovered
                    }
                    HoverHandler { id: readerHover }
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
                    // An invisible footer still counts toward contentHeight.
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
            // Release the tone level first; while the tone popup is up the
            // owner is "tone".
            timeline.releaseTransientInteraction("tone")
            timeline.releaseTransientInteraction("picker")
            targetEventId = ""
        }
        // The skin-tone popup owns row interaction while it is open.
        onToneOpened: timeline.claimTransientInteraction("tone")
        // Both close orders converge: if the picker closes first it releases
        // "tone" itself and this handler matches nothing.
        onToneClosed: timeline.releaseTransientInteraction("tone", "picker")
        onEmojiChosen: (emoji) => {
            if (targetEventId !== "")
                app.composer.reactTo(targetEventId, emoji)
        }
    }
    MemberProfilePopover {
        id: senderProfilePopover
        parent: Overlay.overlay
        anchors.centerIn: parent
        onOpened: timeline.claimTransientInteraction("profile")
        onClosed: timeline.releaseTransientInteraction("profile")
    }
    // Room-scoped invite. Separate from Space Home's instance so neither
    // inherits the other's target room.
    InvitePeopleDialog {
        id: roomInviteDialog
        objectName: "roomInviteDialog"
        parent: Overlay.overlay
    }

    // MSC3030 jump to date. Stays open until the server answers, since a server
    // without the endpoint is an outcome the user must be told about.
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

    // Closes every floating surface anchored to or captured from a timeline
    // row. Also called when content is discarded without a room change (the
    // jump-to-live history trim resets the model), where no switch signal
    // fires.
    function closeRowAnchoredSurfaces() {
        sharedReactionPicker.close()
        senderProfilePopover.close()
        receiptListPopover.close()
        // The viewer holds decoded pixels and a stale entries snapshot.
        imageViewer.close()
        // Reset transient row-interaction ownership, in case a surface was
        // destroyed without releasing its claim; otherwise the action bar could
        // never show again.
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
    // Screenshot-demo hooks. Inert in a non-demo build (null target).
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenMessageContextMenu() {
            var tries = Math.min(timeline.count, 40)
            // Prefer a plain-text row sent by the demo account: canEditEvent()
            // is the same gate the Edit menu item uses, so the menu shows Edit
            // and Delete.
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
            // Fall back to any instantiated row, newest first.
            // openContextMenu() ignores virtual rows and rows without an event
            // id.
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
            // A fictional demo member; the popover only renders caller-supplied
            // fields.
            timeline.openSenderProfile({
                userId: "@maya:lightning.example",
                displayName: "Maya Chen",
                membership: "joined",
                role: "",
                avatarUrl: "mxc://lightning.example/avatar-maya",
                isOwn: false
            })
        }
        // Drives the find bar through the real openFind() path. Setting the
        // text after openFind() fires onTextChanged -> updateSearch, as typing
        // does.
        function onDemoOpenFindBar(query) {
            root.openFind()
            findField.text = query
        }
        // Opens Room Information and Find in History mode through the real
        // controls, so a capture cannot show a state the app has no path to.
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
    // GIF save/unsave feedback via the same auto-clearing banner as Save As.
    // `message` is already translated and ready to display.
    Connections {
        target: app.gif.starredStore
        // Failures only: on success the star itself fills or empties.
        // "already_starred" is reported as ok; MessageDelegate refreshes the
        // star regardless of `ok`.
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
        // Below 660px the 340px panel and the 320px timeline minimum cannot
        // coexist, so the thread surface takes the pane; the room stays alive
        // underneath.
        visible: !(root.threadSurfaceOpen && root.width < 660)
                 && !(root.searchOpen && root.width < 700)
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 320
        spacing: 0

        // Room header: 60px, 34px avatar, 34x34 icon buttons.
        Rectangle {
            id: roomHeaderBand
            objectName: "roomHeaderBand"
            Layout.fillWidth: true
            implicitHeight: AppTheme.headerBandHeight
            // The timeline paints after this band, so anything drawn outside it
            // would sit over the messages and be unclickable (e.g. a topic with
            // newlines).
            clip: true
            // The pane tone, not the card tone, so the composer card lifts off
            // it and the header runs continuous with the room list.
            color: AppTheme.sidebar
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing20
                anchors.rightMargin: AppTheme.spacing20
                spacing: AppTheme.spacing12

                // When the header cannot fit everything, the title wins: it is
                // the only indication of which room this is, while every action
                // icon can fold into the overflow menu. The icon row cannot
                // shrink (a nested RowLayout's minimum is the sum of its
                // children), so without folding the whole shortfall lands on
                // the title.
                //
                // The budget reads header.width (from anchors.fill), never a
                // layout-derived width, so it cannot feed back into the row it
                // sizes.

                // The title's floor (about fifteen characters in its own font)
                // plus the encryption lock and its gap on the same row.
                readonly property real identityFloor:
                    Math.round(AppTheme.scaled(AppTheme.textTitle) * 9)
                    + AppTheme.spacingS + 14
                // How many 34px icons fit beside the title at its floor. Width
                // 0 during load means no limit, so the overflow button does not
                // flash on open.
                readonly property int actionSlots: {
                    if (width <= 0)
                        return 99
                    // 34 is IconButton's "lg" size.
                    var step = 34 + AppTheme.spacing6
                    // `size`, not `width`: the layout assigns width, so reading
                    // it here would make the budget depend on the row it sizes.
                    var budget = width
                                 - (roomHeaderAvatar.visible
                                    ? roomHeaderAvatar.size + spacing : 0)
                                 - 2 * spacing
                                 - identityFloor
                    return Math.max(
                        0, Math.floor((budget + AppTheme.spacing6) / step))
                }

                Avatar {
                    id: roomHeaderAvatar
                    objectName: "roomHeaderAvatar"
                    visible: app.currentRoomId !== ""
                    size: 34
                    squareRadius: 9
                    name: root.currentRoom.name || app.currentRoomId
                    mxc: root.currentRoom.avatarUrl || ""
                    colorKey: root.currentRoom.identityColorKey
                              || app.currentRoomId
                    // People/DMs are circles; rooms and Spaces are rounded
                    // squares.
                    circle: root.currentRoom.isDirect === true

                    // The peer's presence, on unambiguous 1:1 DMs only (the
                    // room list's rule: identityColorKey is the partner's MXID
                    // exactly then). A group DM or room watches nobody and
                    // renders nothing. Kept inside the avatar's bounds so it
                    // never reaches the title beside it or the band's clip.
                    PresenceDot {
                        objectName: "roomHeaderPresenceDot"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        dotSize: 12
                        ring: roomHeaderBand.color
                        hoverStatus: true
                        userId: root.currentRoom.isDirect === true
                                && (root.currentRoom.identityColorKey || "")
                                       .charAt(0) === "@"
                                ? root.currentRoom.identityColorKey : ""
                    }
                }
                ColumnLayout {
                    objectName: "roomHeaderIdentity"
                    spacing: 2
                    Layout.fillWidth: true
                    // Lets a narrow header take width from the name, which
                    // elides, rather than from the action icons, which the band
                    // would clip.
                    Layout.minimumWidth: 0
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Label {
                            objectName: "roomHeaderTitle"
                            // Untrusted text: never markup.
                            textFormat: Text.PlainText
                            text: {
                                if (root.currentRoom.name)
                                    return root.currentRoom.name
                                if (app.currentRoomId !== "")
                                    return app.currentRoomId
                                // With no room open, Space Home shows the
                                // Space's name.
                                if (app.spaces
                                        && app.spaces.activeSpaceId.length > 0
                                        && app.spaces.activeSpaceId.charAt(0) === "!")
                                    return app.spaces.spaceName(
                                        app.spaces.activeSpaceId) || qsTr("Space")
                                return qsTr("Home")
                            }
                            color: AppTheme.text
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                            font.weight: AppTheme.weightBold
                            elide: Label.ElideRight
                            maximumLineCount: 1
                            // fillWidth so a narrow header can shrink the name;
                            // maximumWidth is the text's own width so a short
                            // name hugs its text and the lock sits beside it.
                            Layout.fillWidth: true
                            // Ceiled: a Layout assigns integer widths, and a
                            // fractional implicitWidth would elide by a
                            // fraction of a pixel.
                            Layout.maximumWidth: Math.ceil(implicitWidth)
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
                        // One line: a topic is free text and often contains
                        // newlines, and Text breaks on an explicit newline
                        // regardless of elide, which would push the header past
                        // its 60px band.
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
                    TapHandler {
                        enabled: app.currentRoomId !== "" && app.roomInfo.supported
                        onTapped: root.toggleRoomInfo()
                    }
                }
                // Named for a geometric test.
                Item { objectName: "roomHeaderSpacer"; Layout.fillWidth: true }
                RowLayout {
                    id: roomHeaderActions
                    objectName: "roomHeaderActions"
                    spacing: AppTheme.spacing6
                    // fillWidth: false, because a nested layout defaults to
                    // true and the band clips, so losing the competition for
                    // width would cut the icons. Deliberately no
                    // `Layout.minimumWidth: implicitWidth`: it is a
                    // self-reference through the layout engine and destabilises
                    // geometry. The identity column's minimumWidth: 0 is what
                    // gives way instead.
                    Layout.fillWidth: false

                    // Declaration order is the drawn order; foldOrder is the
                    // priority order, least important first, ranked by how else
                    // the action is reachable. Room info is also opened by
                    // tapping the header identity; pinned, members and search
                    // have panel sections or shortcuts; voice call has no other
                    // route and folds last. Everything folded appears in the
                    // overflow menu.
                    readonly property var foldOrder: [
                        "roomInfoButton", "pinnedMessagesButton",
                        "memberPanelButton", "timelineSearchButton",
                        "threadsViewButton", "startVoiceCallButton"]
                    // The buttons themselves, so `available` is read from the
                    // one place each rule is written. A property read inside a
                    // binding is tracked; a function call would not be.
                    readonly property var actionButtons: [
                        startVoiceCallButton, pinnedMessagesButton,
                        threadsViewButton, timelineSearchButton,
                        memberPanelButton, roomInfoButton]
                    // Each button declares `available` (its own gate), `folded`
                    // (this list), `actionLabel` (shared by its accessible name
                    // and overflow row) and `visible: available && !folded`.
                    // The keyboard shortcut follows `available`, so a folded
                    // action still works.
                    readonly property var foldedActions: {
                        var live = []
                        for (var i = 0; i < actionButtons.length; ++i)
                            if (actionButtons[i].available)
                                live.push(actionButtons[i].objectName)
                        var slots = header.actionSlots
                        if (live.length <= slots)
                            return []
                        // One slot is the overflow button.
                        var keep = Math.max(0, slots - 1)
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
                        // 1:1 DMs only for the legacy lane, since m.call.invite
                        // rings every member. Idle/Ended only: one call at a
                        // time. AppController decides the lane (MatrixRTC where
                        // available, legacy 1:1 as the audio-only DM fallback);
                        // the button is absent when neither can carry a call.
                        property bool available:
                            app.currentRoomId !== ""
                            && app.canStartCall(app.currentRoomId)
                            // canStartCall is a Q_INVOKABLE, so the binding
                            // records no dependency; callGateRevision
                            // re-evaluates it when RTC state lands.
                            && app.callGateRevision >= 0
                            && !app.groupCall.active
                            && (app.calls.state === CallController.Idle
                                || app.calls.state === CallController.Ended)
                        // Visibility already asks whether either lane can carry
                        // a call, so the button is never shown dead.
                        enabled: true
                        iconName: "call"
                        property string actionLabel: qsTr("Start a voice call")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Start a voice call")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: app.startCall(app.currentRoomId, false)
                    }
                    // Shown only when the room has pins.
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
                        property string actionLabel: qsTr("Threads")
                        Accessible.name: actionLabel
                        ToolTip.text: qsTr("Threads in this room")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleThreadSurface()
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
                    // Overflow button: carries every action that did not fit.
                    // Drawn last so the remaining icons keep their positions.
                    IconButton {
                        id: roomHeaderOverflowButton
                        objectName: "roomHeaderOverflowButton"
                        visible: roomHeaderActions.foldedActions.length > 0
                        iconName: "more_vert"
                        active: roomHeaderOverflowMenu.opened
                        Accessible.name: qsTr("More room actions")
                        ToolTip.text: qsTr("More room actions")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: roomHeaderOverflowMenu.popup(
                            roomHeaderOverflowButton, 0,
                            roomHeaderOverflowButton.height + AppTheme.spacing4)
                    }
                    // A Popup is not an Item, so it costs the row no width.
                    // Each entry binds to its button; `folded` is only set on
                    // an available action.
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
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // In-call controls under the room header, for both the legacy 1:1 lane
        // and MatrixRTC. Zero height when no call is live in this room.
        CallHeaderBar {
            objectName: "timelineCallHeaderBar"
            Layout.fillWidth: true
            // Opens the room's side panel rather than a second participant
            // list.
            onParticipantsRequested: root.infoOpen = !root.infoOpen
        }

        // The call panel sits at the top of the column with the timeline
        // scrolling beneath it and a draggable divider between. It takes a
        // bounded, explicitly assigned height rather than fillHeight, so the
        // two never split the column.
        //
        // The reader's position does not move when a call starts: the timeline
        // is a rotated Flickable whose content is pinned to the bottom edge,
        // and only its height changes. Shrinking the panel far enough makes the
        // Flickable clamp, like a window resize. Growing the timeline can enter
        // the near-top pagination band and trigger a backfill, as making the
        // window taller does.
        Loader {
            id: callStageHost
            objectName: "timelineCallStageHost"
            Layout.fillWidth: true
            // Bounded, never fillHeight: the timeline below keeps that.
            Layout.preferredHeight: active ? root.callPanelHeight : 0
            active: root.callStageOwnsColumn
            visible: active
            sourceComponent: CallStage {
                collapsed: root.callPanelCollapsed
                // Opens the room's side panel, like the header bar's button.
                onParticipantsRequested: root.infoOpen = !root.infoOpen
                onCollapseToggled: root.callPanelCollapsed = !root.callPanelCollapsed
            }
        }

        // Divider that resizes the call panel. Hand-rolled rather than a
        // SplitView to avoid restructuring the timeline's ColumnLayout. A
        // release moves nothing and emits no heightChanged, so the height is
        // committed on the falling edge of the drag's `active`.
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
                        // Falling edge: the only moment that can commit the
                        // gesture.
                        root.callPanelUserHeight =
                                root.clampCallPanelHeight(root.callPanelHeight);
                    }
                }
                // activeTranslation resets on each new drag, which suits
                // base-plus-delta.
                onActiveTranslationChanged: {
                    if (!dividerDrag.active)
                        return;
                    root.callPanelUserHeight = root.clampCallPanelHeight(
                                root.callPanelDragBase
                                + dividerDrag.activeTranslation.y);
                }
            }
        }

        // "N people in call". A Layout child so persistent state reflows the
        // timeline rather than covering messages. Observational only; zero
        // height with no call.
        RoomCallBanner {
            objectName: "timelineRoomCallBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: visible ? AppTheme.spacing8 : 0
            roomId: app.currentRoomId
        }

        // Room upgrade banner. Lightning does not follow a tombstone
        // automatically: the old room stays readable and the successor is
        // offered, since switching discards navigation and draft state and
        // anyone with the power level can send the tombstone.
        //
        // A Layout child so it reflows the timeline. The wording is Lightning's
        // own; the tombstone's free-text body never crosses the FFI.
        Rectangle {
            id: roomUpgradeBanner
            objectName: "roomUpgradeBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            // A room can be both a successor and a predecessor, so the rows are
            // independent.
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

                // Shown in the old room, so the user sees why Continue failed
                // without having been moved.
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

                    // Carries its own glyph because both rows can be visible at
                    // once.
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
                        // No join step: the user was already in the
                        // predecessor.
                        onClicked: app.roomUpgrade.goToPredecessor()
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open the previous room")
                    }
                }
            }
        }

        // Find bar, a floating card with a fixed compact height so opening or
        // focusing it never reflows anything else. A Layout child rather than
        // an overlay: the timeline's onHeightChanged already preserves the
        // reading position across a find-bar resize, while an overlay would
        // cover the newest message.
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
                // Loaded-messages find vs history search.
                SegmentedControl {
                    objectName: "findModeToggle"
                    visible: root.findHistoryAvailable
                    dense: true
                    // Explicitly false: a nested RowLayout defaults to
                    // fillWidth and would leave a gap before the field.
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
                            // Refresh index stats when the bar opens; otherwise
                            // the coverage line can read "nothing indexed"
                            // above results the index just returned.
                            app.messageSearch.refreshIndexStats()
                            // Prefer the local index: it works in encrypted
                            // rooms and needs no round trip. Server search is
                            // chosen explicitly below.
                            app.messageSearch.source =
                                app.messageSearch.localAvailable
                                ? "local" : "server"
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
                // Jump to date lives with Find: both answer "where is that
                // message", and the header row is already crowded.
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
                            // The controller is shared with the global search
                            // dialog, which rescopes it; re-assert this room on
                            // every dispatch.
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

            // History results, plus what the search covers. A local index
            // answers from what it holds, so show the indexed count and offer
            // to index further back; otherwise "no results" reads as "never
            // said".
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
                        // Stats arrive on their own signal, so a zero count
                        // while results are shown is not an empty index. Say
                        // nothing rather than something false.
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
                    // One at a time: two runs would race for the same rows.
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
            // Server search stays reachable where the server can read the room.
            // A choice, not a fallback, so "no results" has one meaning.
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
                            // Paginates until the event is loaded, then centres
                            // and highlights it.
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
                                // Untrusted text: never markup.
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
                                        Qt.locale(), Locale.ShortFormat)
                                }
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        Label {
                            // Untrusted text: never markup.
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

        // Timeline
        Item {
            // Stays visible through a call; the call panel has a bounded
            // height.
            visible: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Solid timeline: every loaded row is a real item in one Column, so
            // contentHeight and row positions are measured. No height
            // estimates, no delegate recycling.
            //
            // This replaced a reversed TableView, which rebuilt the whole
            // viewport on any insert (RebuildOption::ViewportOnly) and needed
            // estimated content heights that disagreed with the real layout.
            // Here older history is a proxy append at the column's tail, so no
            // positioned item moves and pagination needs no contentY
            // correction. The cost is that the whole loaded window is
            // instantiated, as in Element; rows arrive ~20 per page and hidden
            // rows take zero height. See docs/timeline-scrolling.md.
            Flickable {
                id: timeline
                // Media band: the row index range allowed to fetch pictures now
                // (MessageDelegate.mediaInBand). Not bound to contentY, which
                // would re-run a comparison in every row on every scroll frame;
                // it moves only on load, reset, resize and gesture settle. An
                // index range because a delegate's own y inside its Loader is
                // 0. Permissive until the first refresh.
                property int mediaBandFirstRow: 0
                property int mediaBandLastRow: 2147483647
                function refreshMediaBand() {
                    if (count <= 0) {
                        mediaBandFirstRow = 0
                        mediaBandLastRow = 2147483647
                        return
                    }
                    var h = Math.max(height, 400)
                    // Never close the band on the newest side: a picture
                    // loading late between the reader and the live edge would
                    // grow below the reader and move them. Only history beyond
                    // 2.5 viewports waits.
                    var last = viewRowAtContentY(contentY + h * 2.5)
                    mediaBandFirstRow = 0
                    mediaBandLastRow = last < 0 ? count - 1 : last
                }
                // Programmatic landings (jump to event, first unread, glides)
                // do not touch the wheel/drag settle timer, so every contentY
                // change restarts this; the band moves 300 ms after the last
                // change.
                Timer {
                    id: mediaBandSettle
                    interval: 300
                    repeat: false
                    onTriggered: timeline.refreshMediaBand()
                }
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
                // Rotated viewport with counter-rotated rows: proxy row 0
                // (newest) sits at the bottom and history extends upward.
                rotation: 180
                flickableDirection: Flickable.VerticalFlick
                // The view keeps laying out under the gate so viewport fill and
                // positioning run against real geometry; the loading surface
                // covers it.
                opacity: presentationReady ? 1 : 0
                // The proxy exposes rows newest-to-oldest; the source stays
                // chronological.
                readonly property int count: rowRepeater.count
                contentWidth: width
                contentHeight: rowColumn.height

                // Row range allowed to activate heavy content. Computed once
                // per turn so each row does two integer comparisons instead of
                // a per-pixel float binding across hundreds of delegates. One
                // viewport of slack on each side.
                property int visibleFirstRow: 0
                property int visibleLastRow: -1
                // A child Timer rather than Qt.callLater: a queued callLater
                // can fire after teardown (room switch, logout) and throw.
                // restart() coalesces requests.
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
                    // A Flickable's left/right margins only widen the flick
                    // range, so inset the content directly. The rotation swaps
                    // left and right, harmless while equal.
                    x: timeline.leftMargin
                    // Rows can be built before the pane has a width, and a
                    // non-positive width measures a wrapped body as enormously
                    // tall. Fall back to a normal column width until then.
                    width: {
                        var available = timeline.width - timeline.leftMargin
                                      - timeline.rightMargin
                        return available > 0 ? available : 640
                    }
                    // Delegates own sender-group spacing so continuations stay
                    // visually joined.
                    spacing: 0

                    // Records how many rows the last positioning pass covered,
                    // so jump paths know when a delegate's y can be trusted
                    // (see navigationGeometryReady()). Nothing binds to it, so
                    // writing it here cannot loop.
                    onPositioningComplete: {
                        timeline.layoutRowsAtLastPass = timeline.count
                        // The Column has re-positioned the current children, so
                        // contentHeight now describes the new snapshot and
                        // presentationGeometryStale may clear. A 0 ms timer is
                        // not equivalent: it could fire while contentHeight
                        // still described the previous room and open the gate
                        // on a one-item snapshot. Unlike onContentHeightChanged
                        // this also fires when the new height equals the old.
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
                            // No attached view exists inside a Repeater, so
                            // inject it. The height-seed/recycling API is
                            // deliberately withheld so the delegate uses its
                            // natural height.
                            timelineView: timeline
                            // An index-range test, never geometry; see
                            // visibleFirstRow.
                            rowOnScreen: index >= timeline.visibleFirstRow
                                         && index <= timeline.visibleLastRow
                            // Same for the media band.
                            mediaInBand: index >= timeline.mediaBandFirstRow
                                         && index <= timeline.mediaBandLastRow
                        }
                    }
                }

                // No height model: the Column measures itself and appends
                // cannot move a positioned item. Lightning owns the scroll
                // position (writes contentY clamped to wheelMinY/wheelMaxY);
                // StopAtBounds keeps a native wheel event from overshooting or
                // rubber-banding past that clamp.
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

                // Read defensively: several suites construct this pane without
                // `app`, and an undefined read would break the wheel handler.
                readonly property bool smoothScrollingEnabled: {
                    // Coerce explicitly: a stub settings object yields
                    // undefined, and assigning undefined to a bool warns (the
                    // GIF picker suites treat that as a failure). Defaults to
                    // true.
                    if (typeof app === "undefined" || !app || !app.settings)
                        return true
                    var v = app.settings.smoothScrolling
                    return v === undefined ? true : !!v
                }

                property bool stickToBottom: true

                // Bottom-follow is latched to user intent: recomputed only on
                // user input (wheel/drag/settle), never on async content
                // growth, so a reader who scrolled up stays disengaged until
                // they return to the end, jump to latest or open a room.
                // Proximity must never re-pin them. The slack is less than one
                // line, so scrolling up to re-read always disengages.
                readonly property real bottomFollowSlack: 8
                function atBottomEdge() {
                    // While the row window hides the live edge, the physical
                    // bottom is not the newest message, so it is not "at
                    // bottom".
                    if (rowWindowSkip > 0)
                        return false
                    return atYBeginning
                           || contentY <= wheelMinY() + bottomFollowSlack
                }
                readonly property int rowWindowSkip:
                    app.timelineView && app.timelineView.windowSkip !== undefined
                    ? app.timelineView.windowSkip : 0

                // Initial-hydration gate: a freshly opened room stays covered
                // by the loading surface until the content fills the viewport
                // or the backend reports the initial fill settled (batch
                // landed, start of history, stopped, failed). The guard timer
                // is only a safety valve for a hung backend. Monotonic per room
                // generation.
                property bool presentationReady: app.currentRoomId === ""
                // Set while a room reset is in flight. Rows are built
                // synchronously, so contentHeight moves during the reset and
                // recomputing the gate then would read the outgoing room's
                // state and open on a one-item snapshot.
                property bool presentationResetPending: false
                // True from a model reset until the Column produces geometry
                // for the new snapshot. While stale, fillsViewport is
                // untrustworthy; the settled/guard paths still open the gate.
                property bool presentationGeometryStale: false
                // Whether the view held rows when the last reset was announced
                // (see onModelAboutToBeReset).
                property bool presentationResetHadRows: false
                function recomputePresentationReady() {
                    if (presentationReady || presentationResetPending)
                        return
                    if (app.currentRoomId === "") {
                        presentationReady = true
                        presentationGuard.stop()
                        return
                    }
                    // contentHeight is only meaningful once the pane has a
                    // height and the Column has re-laid-out since the last
                    // reset; before that it still describes the outgoing rows.
                    var fillsViewport = count > 0 && height > 0
                                        && !presentationGeometryStale
                                        && contentHeight >= height - 1
                    if (fillsViewport || app.pagination.initialContentSettled) {
                        presentationReady = true
                        presentationGuard.stop()
                        // When following the latest message, present with one
                        // deferred end-anchor; otherwise the anchor restore has
                        // already positioned the saved spot.
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
                        // The fill's next page follows the controller going
                        // idle rather than a 250 ms retry timer; rows land
                        // before the controller finishes a batch, so the timer
                        // added one wait per page on first open.
                        if (!app.pagination.busy)
                            timeline.maybeFillViewport()
                    }
                }

                // Persistent viewport anchor: the first visible stable item id
                // plus its pixel offset. Every coalesced content-height change
                // re-aligns to it, so async row growth (media, late decryption,
                // previews, profiles) does not move the message being read.
                // This is the only position-preserving mechanism; bottom-pinned
                // and in-motion states have their own.
                property string viewAnchorId: ""
                property real viewAnchorOffset: 0
                // The anchor row's content y at its last measurement. Re-based
                // on every application so the next delta only covers new
                // growth.
                property real viewAnchorLastY: 0
                // Row count at the last anchor measurement. Distinguishes an
                // insertion (the displaced-anchor path) from the reader merely
                // scrolling away from the anchor, where re-capturing is the
                // cheap, correct answer.
                property int viewAnchorCount: 0
                // Row index and content height at the last measurement. When an
                // insertion displaces the anchor out of the cache, a rising row
                // index proves rows were inserted above and the height growth
                // says how much, so the correction needs no delegate built
                // (building one sweeps delegate creation and media requests
                // across every row it passes).
                property int viewAnchorRow: -1
                property real viewAnchorContentHeight: 0
                // Diagnostic only: originY also moves when the height estimate
                // is revised, so recording it beside the row index lets a trace
                // tell the two causes apart.
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
                        // Diagnostics: distinguish "ran with nothing to
                        // correct" from "never engaged". The two early returns
                        // are counted separately: no anchor captured yet, and a
                        // correction scheduled while scrolled up then dropped
                        // on return to bottom. An empty id wins when both hold.
                        if (scrollTrace) {
                            if (viewAnchorId === "")
                                diagNoAnchorReturns += 1
                            else
                                diagStickToBottomReturns += 1
                        }
                        return
                    }
                    // The single anchor-correction mechanism for every change
                    // that moves the anchor row (pagination prepend,
                    // re-measurement, late decryption, previews). It runs once
                    // per coalesced onContentHeightChanged regardless of cause.
                    // Do not add a second capture/restore path for pagination
                    // or a pagination stand-down guard here: two mechanisms
                    // writing contentY is what caused the earlier scroll bugs.
                    // See docs/timeline-scrolling.md.
                    var row = viewRowForStableId(viewAnchorId)
                    var it = row >= 0 ? itemAtViewRow(row) : null
                    var anchorY = anchorPositionForItem(it)
                    // Diagnostics: on a proven insertion before the anchor,
                    // pair the originY shift with the whole-content delta so a
                    // trace can compare the two.
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
                        // Displaced by an insertion above the reader (the row
                        // index rose). Idle only: during active input this
                        // estimate can cancel an entire wheel gesture, so it
                        // falls through to the measurement-only re-capture
                        // below.
                        //
                        // Positive net only. `grew` is a whole-content delta; a
                        // negative value is dominated by estimate noise or a
                        // below-viewport shrink, which this cannot correct.
                        // Skipping is the better approximation, and a skipped
                        // shrink is absorbed at settle by captureViewAnchor().
                        // viewAnchorLastY is not advanced when skipped, or the
                        // returning delegate would produce a jump. An appended
                        // live row in the same turn is included too; accepted
                        // as one row's worth of error.
                        var grew = contentHeight - viewAnchorContentHeight
                        // Diagnostics: record every entry, including skipped
                        // negatives, to show whether |grew| stays proportional
                        // to insertedRows. Gated on scrollTrace so it survives
                        // past the triggering gesture.
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
                        // Unresolvable: the id no longer exists (redaction,
                        // local-echo id change) or a native drag owns the view.
                        // Re-measuring writes no position, so it is safe
                        // mid-gesture. Diagnostics split by whether the id
                        // still resolves: row < 0 is a stale id; row >= 0 is a
                        // delegate evicted with no proof of insertion. Counters
                        // are not reset on a room switch.
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
                        // Moving contentY by the anchor row's own movement
                        // holds the message on screen while rows around it
                        // resize.
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
                        // No write while input owns the viewport. Applying this
                        // delta was tried twice and pulled the reader both
                        // ways, during loading and during ordinary scrolling:
                        // anchorY also moves for reasons that are not
                        // displacement of the reader. Do not re-enable it
                        // without a measurement that separates the two. Accept
                        // the live layout position and re-base; settle captures
                        // the final position.
                        captureViewAnchor()
                        return
                    }
                    // Idle: nothing competes for contentY, so an absolute
                    // restore to the captured offset is safe and equivalent to
                    // the relative form.
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

                // MessageDelegate view contract: the room timeline resolves
                // stable-id actions against app.timeline and never suppresses a
                // row as a pinned thread root.
                property var timelineModel: app.timeline
                // Every jump/search/permalink path calls this before addressing
                // a row by id. It must restore the live edge: releaseAll()
                // lifts the pacing cap but keeps the row window's skip, so a
                // jump to a recent message would find no row. clearWindow()
                // resets the skip and releases the backlog.
                function releasePendingRows() {
                    if (!app.timelineView)
                        return
                    if (app.timelineView.clearWindow)
                        app.timelineView.clearWindow()
                    else if (app.timelineView.releaseAll)
                        app.timelineView.releaseAll()
                }
                // View row <-> source row, anchored on the source total minus
                // the row window's skip, never on `count`: the proxy paces new
                // history out over a few frames, and deriving from `count`
                // would renumber visible rows while a page drains. Getting this
                // wrong fails silently ("no such row").
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
                // Every loaded row is instantiated, so this is a direct lookup.
                function itemAtViewRow(row) {
                    return row < 0 || row >= count
                            ? null : rowRepeater.itemAt(row)
                }
                // Binary search over real geometry; rows are in ascending y.
                // Hidden rows have zero height and are never the answer.
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
                    // of the range.
                    return viewRowAtContentY(
                        contentY + Math.max(0, height - topMargin - 1))
                }
                // Rotated view: a row's logical bottom edge is its physical
                // top. Anchor that edge so height changes keep the message
                // under the reader.
                function anchorPositionForItem(item) {
                    return item ? item.y + item.height : 0
                }
                // Row 0 is the newest and sits at content y 0 (the physical
                // bottom), so following the latest is just the low bound.
                function positionViewAtLatest() {
                    contentY = wheelMinY()
                }
                // How far into the viewport a navigation target lands. A third
                // of the viewport in shows context on both sides. Uses the same
                // measured geometry and clamp as positionViewAtViewRow(); not a
                // second anchor mechanism.
                readonly property real navigationInsetFraction: 0.33
                function positionViewAtNavigationTarget(row) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return false
                    // Put the row's physical top at the viewport's physical
                    // top, then move it down by the inset.
                    var target = anchorPositionForItem(item) - height
                                 + topMargin + height * navigationInsetFraction
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    contentY = target < lo ? lo : (target > hi ? hi : target)
                    return true
                }
                // Navigation diagnostics, counts only (never an event id). Both
                // failure paths used to return silently.
                // diagWindowUnmeasuredRows: rows still measuring zero after a
                // layout flush when the row window corrected contentY; non-zero
                // means a window move threw the reader.
                property int diagWindowUnmeasuredRows: 0
                property int diagNavigationLandings: 0
                property int diagNavigationUnresolved: 0
                // Jumps the reader overrode by scrolling before they landed. A
                // large count relative to diagNavigationLandings means targets
                // take too long to build.
                property int diagNavigationAbandoned: 0
                // Geometry half of onTargetLocated. releasePendingRows() can
                // insert hundreds of rows in one turn, and a row created this
                // turn is unmeasured until polish (y == 0, height == 0), so
                // landing immediately would clamp to the newest end.
                // navigationGeometryReady() requires the row to be measured and
                // the last Column layout pass to cover the current row set.
                //
                // Then write contentY exactly once; no correction loop. A newer
                // jump replaces a pending one.
                property int navigationPendingRow: -1
                // The target's stable id, authoritative while the landing
                // waits. navigationPendingRow is only the fallback when the
                // model has no id.
                property string navigationPendingId: ""
                property real navigationPendingOffset: 0
                property bool navigationPendingHighlight: false
                property int navigationPendingAttempts: 0
                // Attempts since the landing was armed, never reset by the
                // convergence re-arm; the ceiling that stops it waiting
                // forever.
                property int navigationTotalAttempts: 0
                // "<rows>/<laidOutRows>" at the previous attempt. A change
                // means the view is still converging and re-arms the budget.
                property string navigationLastShape: ""
                // Row count covered by the Column's last completed positioning
                // pass (from its positioningComplete signal). Differing from
                // `count` means every y is from the previous shape.
                property int layoutRowsAtLastPass: 0
                function navigationGeometryReady(item) {
                    if (!item)
                        return false
                    // A deliberately hidden row is measured at zero and never
                    // grows, so do not wait for it.
                    if (item.height <= 0 && item.visible)
                        return false
                    return layoutRowsAtLastPass === count
                }
                // ~200 ms of frames, counted in ticks rather than wall clock so
                // a slow tick does not burn the budget.
                readonly property int maxNavigationLandingAttempts: 12
                // Absolute ceiling, ~2 s. While the reader scrolls, the row set
                // and layout pass change constantly and would re-arm the budget
                // forever, so the landing would eventually fire mid-gesture as
                // a teleport.
                readonly property int maxNavigationLandingTicks: 120
                Timer {
                    id: navigationLandingTimer
                    interval: 16
                    repeat: false
                    onTriggered: timeline.tryLandNavigationTarget()
                }
                // One backward page per tick while hunting the read marker. 240
                // ms because each tick is a network pagination. See
                // goToFirstUnread().
                Timer {
                    id: firstUnreadRetryTimer
                    interval: 240
                    repeat: false
                    onTriggered: timeline.landOnFirstUnread()
                }
                // Genuine input abandons a jump that has not landed, or it
                // could yank the view later. Called only from pointer input
                // (wheel, drag, flick, autoscroll), never from a programmatic
                // write.
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
                // True once the reader has moved this room's view. Cleared on
                // model reset.
                property bool readerControlledSinceReset: false
                // Single entry point for "the reader is driving". Also reaches
                // the controller: a scroll-anchor restore can spend seconds
                // paginating before it arms a landing, which would then
                // teleport the reader back to where the room opened.
                function noteReaderTookControl() {
                    readerControlledSinceReset = true
                    if (app.pagination)
                        app.pagination.cancelNavigation()
                    abandonNavigationLanding()
                    // The unread hunt pages the timeline too, so a gesture
                    // cancels it.
                    firstUnreadPagesLeft = 0
                    firstUnreadRetryTimer.stop()
                }
                function beginNavigationLanding(row, pixelOffset, highlight) {
                    // Hold the target by stable id, not row number: a
                    // pagination batch during the wait renumbers source rows.
                    // Resolved back to a row on each attempt via
                    // rowForStableId().
                    navigationPendingId = app.timeline
                                          ? app.timeline.stableIdAt(row) : ""
                    // Fallback for a model that cannot give a stable id.
                    navigationPendingRow = row
                    navigationPendingOffset = pixelOffset
                    navigationPendingHighlight = highlight
                    navigationPendingAttempts = 0
                    navigationTotalAttempts = 0
                    navigationLastShape = ""
                    // Try immediately: an already-exposed target lands this
                    // turn.
                    tryLandNavigationTarget()
                }
                function tryLandNavigationTarget() {
                    if (navigationPendingRow < 0)
                        return false
                    // Re-derive the row on every attempt.
                    const row = navigationPendingId !== ""
                                && app.timeline
                                ? app.timeline.rowForStableId(
                                      navigationPendingId)
                                : navigationPendingRow
                    const viewRow = row >= 0 ? viewRowForSourceRow(row) : -1
                    let item = viewRow >= 0 ? itemAtViewRow(viewRow) : null
                    // Measured, but the last positioning pass covered a
                    // different row set. While history streams in, a matching
                    // pass may never arrive before the budget runs out, so
                    // request one: forceLayout() synchronously flushes existing
                    // children without building anything. Once per attempt, and
                    // only with a measured item.
                    if (item && !navigationGeometryReady(item)
                            && (item.height > 0 || !item.visible)) {
                        rowColumn.forceLayout()
                        item = itemAtViewRow(viewRow)
                    }
                    if (!navigationGeometryReady(item)) {
                        // Bound on convergence, not wall clock: under load the
                        // timer keeps firing while nothing progresses. The
                        // budget is only spent on attempts that saw no change
                        // in the row set or laid-out row set, so an impossible
                        // target still gives up promptly.
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
                        ++diagNavigationUnresolved
                        // Report instead of returning silently. Counts and view
                        // shape only, never an event id; the geometry fields
                        // separate "never built" from "never measured".
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
                                     // Ticks well under the ceiling: the target
                                     // can never be measured. At the ceiling:
                                     // the view never stopped changing.
                                     + " ticks=" + navigationTotalAttempts)
                        return false
                    }
                    // A height that settled after the last pass (image, link
                    // preview) leaves a polish pending and every y below it
                    // short. The positioner's forceLayout() flushes that
                    // synchronously, builds nothing, and runs once per jump.
                    rowColumn.forceLayout()
                    const pixelOffset = navigationPendingOffset
                    const highlight = navigationPendingHighlight
                    navigationPendingRow = -1
                    navigationPendingId = ""
                    cancelWheelMotion()
                    stickToBottom = false
                    if (highlight) {
                        positionViewAtNavigationTarget(viewRow)
                    } else {
                        positionViewAtViewRow(viewRow, false)
                        contentY = anchorPositionForItem(item) + pixelOffset
                    }
                    ++diagNavigationLandings
                    saveRoomPosition()
                    captureViewAnchor()
                    return true
                }
                function positionViewAtViewRow(row, centered) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return
                    // Place the row's physical top at the viewport's physical
                    // top (or middle when centering), on measured geometry.
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

                // The message whose action toolbar is pinned open by a click;
                // one at a time. Keyed by SDK item id or event id.
                property string pinnedActionsKey: ""
                // The row under the pointer. Only one action bar may show: a
                // pinned row keeps its bar only while nothing else is hovered.
                property string hoveredActionsKey: ""
                property bool emojiPickerOpen: false
                // Single owner of transient row interaction: "" or one of
                // "picker" | "tone" | "menu" | "profile" | "readers" |
                // "viewer". While set, no row shows its action bar and hover
                // cannot claim one. Not solved with z: a covered bar is still
                // hit-testable.
                property string transientInteractionOwner: ""
                function claimTransientInteraction(owner) {
                    if (!owner || owner.length === 0)
                        return
                    // Clear, not cover: the bar is gone, not merely hidden.
                    hoveredActionsKey = ""
                    pinnedActionsKey = ""
                    transientInteractionOwner = owner
                }
                // Release only what you own: the tone popup and its picker
                // close in an order the popups decide.
                function releaseTransientInteraction(owner, fallback) {
                    if (transientInteractionOwner !== owner)
                        return
                    transientInteractionOwner = fallback ? fallback : ""
                }
                // Guarded here rather than in the delegate's HoverHandler so
                // every writer of the key is covered.
                onHoveredActionsKeyChanged: {
                    if (transientInteractionOwner !== ""
                        && hoveredActionsKey !== "")
                        hoveredActionsKey = ""
                }
                // Drives the link-preview privacy gate in each MessageDelegate.
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

                // Delegate entry points into the media UI, kept on the view so
                // delegates need no external ids.
                property var openImage: function(mediaKey, httpUrl) {
                    imageViewer.openFor(mediaKey || "", httpUrl)
                }
                // Shared picker / profile entry points; the event id is
                // captured at open.
                property var openReactionPicker: function(eventId, point) {
                    if (!eventId || eventId.length === 0)
                        return
                    sharedReactionPicker.targetEventId = eventId
                    sharedReactionPicker.anchorPoint = point
                    sharedReactionPicker.open()
                }
                // Makes the reaction picker and a message context menu mutually
                // exclusive: both are Popup.Item in the same overlay, so the
                // last opened covers the other, and z cannot fix it. Lives on
                // the view because delegates only reach the pane through
                // `timelineView`. Contract: MessageDelegate.openContextMenu()
                // calls this before assigning menuEventId.
                property var closeTransientRowSurfaces: function() {
                    root.closeRowAnchoredSurfaces()
                }
                property var openSenderProfile: function(member) {
                    senderProfilePopover.openFor(member)
                }
                // Shared reply navigation. Delegates reach the view only
                // through `timelineView`, so the same reply preview works in
                // the room and the thread panel.
                // PaginationController::jumpToEvent remains the single history
                // loader.
                property var navigateToEvent: function(eventId) {
                    if (!eventId || eventId.length === 0)
                        return
                    root.stopAutoscroll()
                    timeline.cancelWheelMotion()
                    app.pagination.jumpToEvent(eventId)
                }
                readonly property string navigationHighlightEventId:
                    app.pagination.highlightedEventId
                // Must live on this Flickable: delegates reach the pane only
                // through `timelineView`, and a pane-root function is
                // unreachable.
                property var openReceiptList: function(readers, totalOthers,
                                                       point) {
                    receiptListPopover.readers = readers || []
                    receiptListPopover.totalOthers = totalOthers
                    receiptListPopover.anchorPoint = point
                    receiptListPopover.open()
                }

                property var beginReplyForEvent: function(eventId) {
                    if (!eventId || eventId.length === 0) return
                    var details = timelineModel.messageDetails(eventId)
                    if (!details.eventId) return
                    var previewText = timelineModel.visibleTextForEvent(eventId)
                    app.composer.beginReply(eventId,
                        details.senderName || details.senderId,
                        (previewText || "").substring(0, 80),
                        timelineModel.mediaKeyForEvent(eventId))
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
                    saveMediaDialog.currentFile = root.suggestedSaveUrl(filename)
                    saveMediaDialog.open()
                }
                // Per-card save feedback: the key being written (indeterminate;
                // saves are atomic) and the last finished key for a brief
                // flash. Keyed, so no card shows another save's outcome.
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
                    // Keyed by the bridge's completion, so overlapping saves
                    // flash the right card.
                    if (!mediaKey || mediaKey === "")
                        return
                    if (saveInFlightKey === mediaKey)
                        saveInFlightKey = ""
                    lastSavedKey = mediaKey
                    lastSaveOk = ok
                    savedFlashTimer.restart()
                }

                // Deferred via Qt.callLater: positioning synchronously in
                // onCountChanged during a reset made DelegateModel cancel an
                // out-of-range index. callLater coalesces requests and
                // re-checks state at fire time.
                function scrollToEndDeferred() {
                    // Never fight an in-flight wheel motion; its settle pass
                    // recomputes follow-latest.
                    if (count > 0 && stickToBottom && !wheelAnimating)
                        positionViewAtLatest()
                }

                // TimelineScrollController supplies wheel speed and keyboard
                // paging.
                property bool wheelAnimating: app.timelineScroll.motionActive

                // True while user input owns the viewport: native drag/flick,
                // wheel/keyboard animation, or touchpad deltas (settle timer).
                // No anchor correction may be written then.
                readonly property bool userScrollActive:
                    moving || wheelAnimating || scrollSettleTimer.running

                // Speculative media work (full-payload prefetch and poster
                // extraction) waits until the view settles: rows swept through
                // during a gesture would otherwise each pull a full payload and
                // write temp files on the GUI thread. userScrollActive includes
                // the 250 ms settle tail. Thumbnails are not gated; they are
                // what the reader is looking at.
                readonly property bool speculativeMediaAllowed:
                    !userScrollActive

                // True only while a drag/flick or wheel/keyboard animation owns
                // contentY. Deliberately not userScrollActive, which includes
                // the settle tail: applyRowWindow() runs inside that timer's
                // onTriggered, where `running` is still true, so that guard
                // would stop the row window from ever applying.
                readonly property bool viewportMotionActive:
                    moving || wheelAnimating

                // True only while Lightning itself drives contentY. Diagnostic
                // only: no active input path receives an anchor write.
                readonly property bool selfDrivenScrollActive:
                    !moving && (wheelAnimating || scrollSettleTimer.running)

                // Per-gesture scroll diagnostics, off unless
                // LIGHTNING_SCROLL_TRACE is set. One summary line per
                // wheel/touchpad gesture at settle. No message content, ids or
                // URLs are logged.
                readonly property bool scrollTrace: app.timelineScroll.scrollTraceEnabled
                property bool diagActive: false
                property int diagEvents: 0
                property int diagPixelEvents: 0
                property int diagAngleEvents: 0
                // diagAnchorCorrections counts only the idle absolute-restore
                // path in maintainViewAnchor(). Counters reset at print, so
                // idle restores between gestures (media hydration, late
                // decryption) carry into the next line. diagGrowthCorrections
                // counts displaced-anchor writes after input went idle. Active
                // deltas are recorded in activeDeferred* and never applied.
                property int diagAnchorCorrections: 0
                property int diagGrowthCorrections: 0
                // Rows instantiated in the Column.
                property int diagRowCount: 0
                // Per-outcome counters for maintainViewAnchor() (displaced,
                // capture fallback, drag deferral, materialized, idle restore),
                // so a real trace can show which branch ran and whether each
                // correction was proportionate.
                //
                // Outcome counters are gated on scrollTrace, not diagActive,
                // and drained only when printed, so a correction landing after
                // settle appears on the next line instead of being lost. Read
                // them as "since the previous line".
                //
                // diagNoAnchorReturns / diagStickToBottomReturns separate
                // "never engaged" from "nothing to correct". A bottom-pinned
                // gesture moves neither; stickToBottom returns count
                // corrections dropped on return to bottom.
                // diagUnresolvedIdFallbacks (id no longer resolves) is split
                // from diagEvictedNoInsertFallbacks (delegate evicted, no
                // insertion proven).
                //
                // The max-abs fields store the signed value of the largest |x|
                // sample.
                property int diagNoAnchorReturns: 0
                property int diagStickToBottomReturns: 0
                property int diagDisplacedFirings: 0
                property real diagDisplacedAppliedSum: 0
                property real diagDisplacedMaxAbsGrew: 0
                property int diagDisplacedMaxAbsGrewRows: 0
                // Paired samples in both directions, so two unrelated firings
                // are not conflated.
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
                        diagWindowApplications = 0
                        diagWindowNewEndExtensions = 0
                    }
                    diagEvents += 1
                    if (isPixel)
                        diagPixelEvents += 1
                    else
                        diagAngleEvents += 1
                }
                // Wall clock spent in one wheel event: distinguishes a large
                // timeline from individually expensive rows.
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
                // applyRowWindow() calls that changed the window this gesture.
                // Zero next to a large rows/srcRows gap means the window is not
                // applying.
                property int diagWindowApplications: 0
                // Newest-end window extensions taken during motion.
                property int diagWindowNewEndExtensions: 0
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
                        // Row window state: `rows` is instantiated, srcRows is
                        // loaded; rows == srcRows with a deep reader means the
                        // window is bounding nothing. winSkip is how many
                        // newest rows are withheld.
                        + " srcRows=" + (app.timeline ? app.timeline.count : -1)
                        + " winSkip=" + rowWindowSkip
                        + " winApplies=" + diagWindowApplications
                        + " winExtendNew=" + diagWindowNewEndExtensions
                        // Non-zero: the row window corrected contentY using
                        // rows that still measured zero after a layout flush.
                        + " winUnmeasured=" + diagWindowUnmeasuredRows
                        // gestureMs is wall clock across the gesture;
                        // worstNotchMs is the slowest single wheel event.
                        // stateRows/stateGroups show how much of a large row
                        // count is collapsed room activity.
                        + " gestureMs=" + Math.round(Date.now() - diagGestureStartMs)
                        + " worstNotchMs=" + Math.round(diagWorstNotchMs)
                        // -1 means unavailable, never zero. Guarded: throwing
                        // here would abort the whole line
                        // (scrollTraceLineIncludesAllPerBranchFields).
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

                // Valid contentY range including scroll margins, the pagination
                // header and a shifted origin.
                function wheelMinY() { return originY - topMargin }
                function wheelMaxY() {
                    var maxY = originY + contentHeight + bottomMargin - height
                    var minY = wheelMinY()
                    return maxY < minY ? minY : maxY
                }
                // Row window: bounds the instantiated rows, since render and
                // polish cost grow with loaded rows. Safety constraints:
                //   * applied only when the reader is settled, never
                //     mid-gesture;
                //   * a generous runway below the reader, so a downward gesture
                //     cannot reach the window's low edge;
                //   * the correction restores the reader's own anchor event's
                //     screen offset, and bails before mutating anything if it
                //     cannot be resolved;
                //   * atBottomEdge() refuses while the window hides the live
                //     edge.
                readonly property int windowRunwayRows: 220   // below reader
                readonly property int windowMarginRows: 120   // above reader
                readonly property int windowMinRows: 320      // never below
                function applyRowWindow() {
                    if (!app.timelineView || !app.timelineView.setWindow)
                        return
                    // Settled only, and not while the timeline is still
                    // presenting. viewportMotionActive rather than
                    // userScrollActive: the only caller is the settle timer's
                    // handler, where the timer still reads as running.
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
                    // Absolute offsets from the newest source row: the proxy's
                    // skip plus the view row.
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
                    // Hysteresis: only move for a change worth a structural op.
                    // wantSkip === 0 is exempt: closing to the live edge
                    // approaches 0 in steps, and the last few rows could
                    // otherwise never be closed.
                    const rows = count
                    if (wantSkip !== 0
                        && Math.abs(wantSkip - skip) < 40
                        && Math.abs(wantRows - rows) < 40)
                        return
                    // Thrash guard: releasing rows at the oldest end can put
                    // the reader inside the near-top band, which would backfill
                    // exactly what was released. Decided on measured height,
                    // since the band is 2.5 viewports and the margin is a row
                    // count. If so, keep the tail and take only the skip
                    // change. These are current view rows: the released tail
                    // starts at (wantSkip - skip) + wantRows.
                    const tailFirst = (wantSkip - skip) + wantRows
                    if (tailFirst < rows) {
                        let tailRelease = 0
                        for (let t = tailFirst; t < rows; ++t) {
                            const tailItem = itemAtViewRow(t)
                            if (tailItem)
                                tailRelease += tailItem.height
                        }
                        // Threshold on the enter band, not the exit one:
                        // crossing into it is what dispatches a backfill, and
                        // the exit threshold over-fires.
                        if (distanceFromTop() - tailRelease
                            <= nearTopEnterDistance)
                            wantRows = rows - (wantSkip - skip)
                    }
                    if (wantSkip === skip && wantRows === rows)
                        return
                    // Hold the reader's own row at the same screen offset,
                    // measured before and after. Summing the heights of
                    // released rows does not work: they are at the far end from
                    // a reader in history and may be unmaterialised or
                    // unmeasured. The anchor is resolved by stable id, so it
                    // survives renumbering, and it is on screen so never
                    // released. Unlike a Qt.callLater snap, forceLayout() below
                    // flushes synchronously, so the anchor is re-read from the
                    // new geometry.
                    const anchorRow = Math.max(0, viewRowAtContentY(contentY))
                    const anchorItem = itemAtViewRow(anchorRow)
                    const anchorEventId = eventIdAtViewRow(anchorRow)
                    // An unresolvable reader row means an incoherent view
                    // (mid-reset); applying a structural change there dumps the
                    // reader. Bail.
                    if (!anchorItem || anchorEventId === "")
                        return
                    const anchorOffset = anchorItem.y - contentY

                    app.timelineView.setWindow(wantSkip, wantRows)
                    // A row created this turn reads zero height until the
                    // positioner has run.
                    rowColumn.forceLayout()

                    const movedRow = viewRowForStableId(anchorEventId)
                    const movedItem = movedRow >= 0 ? itemAtViewRow(movedRow)
                                                    : null
                    if (movedItem) {
                        contentY = movedItem.y - anchorOffset
                    } else {
                        // Should be impossible for an on-screen row; count it
                        // rather than apply a correction computed from nothing.
                        ++diagWindowUnmeasuredRows
                    }
                    ++diagWindowApplications
                    updateStickAndPaginate()
                    captureViewAnchor()
                }

                // followStateApplies: false for an input event that could not
                // move the position (a notch into a bound it already sits on);
                // such an event must not change follow-latest, or a room too
                // short to scroll would show a jump pill that never clears. The
                // near-top check still runs: a reader pinned at the oldest row
                // wants more history.
                function updateStickAndPaginate(followStateApplies) {
                    if (followStateApplies !== false)
                        stickToBottom = atBottomEdge()
                    // Edge-latched: re-arms the bounded backfill once per
                    // approach to the top.
                    checkNearTopEdge(true)
                }
                // Can input in this direction move the position? towardsOlder
                // is increasing contentY. Evaluated before dispatch, because
                // the wheel engine clamps its own target and reports an active
                // motion either way.
                function wheelCanMove(towardsOlder) {
                    return towardsOlder ? contentY < wheelMaxY() - 0.5
                                        : contentY > wheelMinY() + 0.5
                }

                // Coalesce near-top backfill onto the next turn: contentY fires
                // every frame and on header height toggles, which produced
                // bursts of near_top requests. The controller still
                // single-flights and bounds empty pages.
                property bool nearTopCheckScheduled: false
                property bool nearTopCheckUserInitiated: false
                // The window's newest edge gives rows back during motion, not
                // only at settle; otherwise a downward gesture stops at the
                // window's synthetic bottom. extendWindowAtNewEnd() returns
                // false when the skip is already 0. The chunk is modest because
                // these rows are built synchronously (3-7 ms each); the
                // settle-time applyRowWindow() releases the surplus again.
                readonly property int windowNewEndExtendRows: 60
                // One viewport of remaining runway, so rows exist before the
                // reader arrives.
                function nearWindowNewEdge() {
                    return rowWindowSkip > 0
                           && contentY - wheelMinY() < Math.max(1, height)
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
                    // Restored rows land at the head and push every kept row by
                    // their summed height. That sum is only exact after layout:
                    // a row created this turn measures zero. Uncorrected, the
                    // glide handler would re-enter every frame and consume the
                    // whole skip.
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
                        contentY = contentY + shift
                        // Translate an in-flight glide rather than cancelling
                        // it, preserving the remaining distance.
                        app.timelineScroll.translateActiveMotion(shift)
                    }
                    // One exact write, no deferred follow-up: a Qt.callLater
                    // snap runs before the Column relayout and reads a stale y.
                    ++diagWindowNewEndExtensions
                    return true
                }

                // True when the window was holding rows back and has now
                // released more. Rows older than the window's oldest exposed
                // row are already loaded.
                function extendRowWindowAtOldEnd() {
                    if (!app.timelineView
                        || !app.timelineView.extendWindowAtOldEnd)
                        return false
                    // The proxy decides: only it can tell the window's cap from
                    // the pacing backlog. `rowWindowSkip + count < total` is
                    // also true during the initial paced reveal and swallowed
                    // near-top requests.
                    return app.timelineView.extendWindowAtOldEnd(
                               windowMarginRows) === true
                }

                function maybeRequestNearTop(userInitiated) {
                    // Capture the anchor at request time if there is none;
                    // otherwise a reader scrolling continuously has no anchor
                    // until settle and prepends compensate nothing. Once per
                    // dispatched request, never per delta.
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

                // Near-top pagination is edge-triggered with hysteresis: latch
                // on entering the band and re-arm only after leaving a wider
                // exit band, so one approach sends at most one user request and
                // the controller bounds continuation through filtered pages. A
                // visible prepend pushes the reader below the exit band,
                // re-arming for the next approach. These are distances from the
                // earliest loaded row, not contentY thresholds. Several
                // viewports of runway keep ahead of a fast upward wheel.
                readonly property real nearTopEnterDistance: height * 2.5
                readonly property real nearTopExitDistance: height * 3.25
                property bool nearTopArmed: true
                // One approach to the top loads a bounded number of rows. The
                // anchor holds the reader on the same row as pages land, the
                // fromTop <= 1 clause bypasses the ratchet, and each productive
                // page re-arms the latch, which together would walk the whole
                // room. Moving past nearTopExitDistance and back starts a new
                // approach with a fresh budget.
                readonly property int nearTopApproachRowBudget: 240
                property int nearTopRowsThisApproach: 0
                // A request budget too, because empty pages (heavily filtered
                // history, e.g. call rooms) never spend the row budget. Larger
                // than rows per page: this is the outer bound on one approach,
                // not a tight cap.
                readonly property int nearTopApproachRequestBudget: 24
                property int nearTopRequestsThisApproach: 0
                // How far the viewport top sits below the earliest loaded row.
                // Proximity bands must use this, never raw contentY: contentY
                // is an offset from originY, which moves as history loads, so a
                // contentY threshold is not a proximity test at all and can be
                // permanently true or false. Live, that made the settle re-arm
                // fire after every gesture and reset the controller's
                // filtered-page bound each time, loading history on every
                // scroll.
                function distanceFromTop() { return wheelMaxY() - contentY }
                // The closest distanceFromTop() reached during this visit to
                // the band: a ratchet, not the distance at the last dispatch.
                // Only motion strictly closer to the top than the reader has
                // already been can fetch, so scrolling up and then slightly
                // down does not load a page. Reset to Infinity on leaving via
                // the exit band or returning to the bottom.
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
                        // The progress gate is at the dispatch, not the settle
                        // re-arm. Consuming the latch requires being pinned
                        // against the top or being strictly closer than the
                        // ratchet, which a downward sample cannot satisfy.
                        if (nearTopArmed
                            && (fromTop <= 1
                                || fromTop < nearTopRequestDistance - 1)) {
                            // Local rows first: with a row window active, the
                            // reader can reach its oldest exposed row while
                            // older history is already loaded. Re-expose
                            // instead of asking the server. Does not consume
                            // nearTopArmed (no request is made). The proxy
                            // paces the extension, and settle re-trims behind
                            // the reader.
                            if (extendRowWindowAtOldEnd())
                                return
                            // The budget bounds the automatic chain and sits
                            // below the local re-exposure, so running out never
                            // strands the reader at the window's boundary. The
                            // ratchet still runs.
                            if (nearTopRowsThisApproach
                                    < nearTopApproachRowBudget
                                && nearTopRequestsThisApproach
                                    < nearTopApproachRequestBudget) {
                                nearTopArmed = false
                                ++nearTopRequestsThisApproach
                                maybeRequestNearTop(userInitiated)
                            }
                        }
                        // Ratchet after the gate has read the old value, on
                        // every in-band sample including ones that did not
                        // dispatch.
                        if (fromTop < nearTopRequestDistance)
                            nearTopRequestDistance = fromTop
                    } else if (fromTop >= nearTopExitDistance) {
                        nearTopArmed = true
                        nearTopRequestDistance = Infinity
                        // Leaving ends the approach; returning later gets a
                        // fresh budget.
                        nearTopRowsThisApproach = 0
                        nearTopRequestsThisApproach = 0
                    }
                }

                function cancelWheelMotion() {
                    // Cancelling retires a pending follow-latest arrival, or it
                    // could later yank a reader who has taken over.
                    followLatestOnArrival = false
                    app.timelineScroll.cancel()
                }

                // Jump to latest glides when near the bottom and stays instant
                // beyond that. Uses the same coalescing motion as the wheel and
                // PageUp/PageDown (app.timelineScroll.animateTo), which every
                // scroll guard already handles. Beyond the threshold a glide
                // would be a blur; Element does not animate this at all.
                property int smoothJumpViewports: 4
                property bool followLatestOnArrival: false
                onWheelAnimatingChanged: {
                    if (wheelAnimating || !followLatestOnArrival)
                        return
                    followLatestOnArrival = false
                    // If the reader redirected mid-glide, that intent wins;
                    // never pin.
                    if (!atBottomEdge())
                        return
                    settleAtLatest()
                }
                // Follow-latest bookkeeping shared by both paths.
                function settleAtLatest() {
                    // The live edge must be exposed first: under an active
                    // window wheelMinY() is the window's newest row, not the
                    // newest message. This is also the fallback when the
                    // history trim refuses, so it cannot rely on the trim's
                    // reset.
                    releasePendingRows()
                    stickToBottom = true
                    // Positioning row zero can re-seed the position frame, so a
                    // surviving anchor baseline would measure across two
                    // frames.
                    viewAnchorId = ""
                    viewAnchorLastY = 0
                    app.pagination.saveFollowingLatest(app.currentRoomId)
                    positionViewAtLatest()
                    Qt.callLater(function() {
                        positionViewAtLatest()
                        app.readReceipts.reevaluate()
                    })
                }

                function beginWheelTo(targetY) {
                    // Keyboard/programmatic motion also owns contentY; end any
                    // autoscroll.
                    root.stopAutoscroll()
                    // Clamp: keyboard callers pass an unclamped target.
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    targetY = targetY < lo ? lo : (targetY > hi ? hi : targetY)
                    app.timelineScroll.animateTo(targetY, contentY, lo, hi,
                                                 height)
                    updateStickAndPaginate()
                    // Upward intent leaves follow-latest; applied last so the
                    // recompute above cannot re-enable it.
                    if (targetY > contentY + 0.5) {
                        stickToBottom = false
                        // Also retire a pending follow-latest arrival:
                        // animateTo() on an active motion does not re-toggle
                        // motionActive, so no arrival handler fires on a
                        // mid-glide redirect.
                        followLatestOnArrival = false
                    }
                    scrollSettleTimer.restart()
                }

                // Keyboard navigation: viewport-relative distances, independent
                // of wheel speed, through the single coalescing motion so key
                // repeats never queue. Only fires while the timeline has active
                // focus.
                function keyboardPage(direction) {   // -1 up, +1 down
                    // Paging is the reader driving. Here rather than in
                    // beginWheelTo(), which programmatic callers also use.
                    noteReaderTookControl()
                    // Rotated: increasing contentY moves toward older rows.
                    beginWheelTo(contentY - direction * height * 0.9)
                }
                // Home jumps directly (like End), then recomputes
                // pagination/follow-latest and saves one settled anchor.
                function goToEarliestLoaded() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    contentY = wheelMaxY()
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
                // Jump to the first unread message. The SDK places a
                // read-marker virtual row from m.fully_read. It has no event
                // id, so this goes through beginNavigationLanding(), which
                // holds a row by stable id across paginations. When the marker
                // is not loaded, page backwards and re-ask, bounded like the
                // reply jump.
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
                        // Offset so the first unread message and the divider
                        // are both on screen.
                        beginNavigationLanding(row, height * 0.25, false)
                        return
                    }
                    if (firstUnreadPagesLeft <= 0
                            || !app.pagination
                            || app.pagination.reachedStart) {
                        firstUnreadPagesLeft = 0
                        return
                    }
                    --firstUnreadPagesLeft
                    app.pagination.requestNearTop(true)
                    firstUnreadRetryTimer.restart()
                }
                function goToLatest() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    // Rotated: the newest row is at wheelMinY().
                    var lo = wheelMinY()
                    var distance = contentY - lo
                    // With a row window active, wheelMinY() is the window's
                    // synthetic edge, not the live edge, so a glide would land
                    // short and leave the jump pill up. A windowed reader is
                    // deep in history anyway; take the far path.
                    if (height > 0 && distance > 0 && rowWindowSkip === 0
                        && distance <= height * smoothJumpViewports) {
                        followLatestOnArrival = true
                        app.timelineScroll.animateTo(lo, contentY, lo,
                                                     wheelMaxY(), height)
                        // The settle pass recomputes pagination and anchoring,
                        // as for a wheel gesture.
                        scrollSettleTimer.restart()
                        return
                    }
                    // Far from the bottom: like Element's jumpToLiveTimeline(),
                    // rebuild at the live edge and discard the paginated
                    // backlog. Only as an explicit user action. The trim
                    // refuses on its own (wrong backend, no room,
                    // mid-pagination, too few rows), falling through to the
                    // ordinary jump. onModelReset() handles the landing. Commit
                    // follow-latest only if the dispatch succeeded; a failed
                    // send never produces a reset.
                    if (app.trimHistoryAndJumpToLive()) {
                        stickToBottom = true
                        app.pagination.saveFollowingLatest(app.currentRoomId)
                        return
                    }
                    settleAtLatest()
                }
                activeFocusOnTab: true
                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_PageUp:
                        // Shift+PgUp jumps to the oldest unread message; plain
                        // PgUp pages up. A Keys case rather than a Shortcut:
                        // PgUp/PgDown/Home/End/Space are reserved in the
                        // registry because a window Shortcut would take them
                        // before the focused item.
                        if (event.modifiers & Qt.ShiftModifier)
                            goToFirstUnread()
                        else
                            keyboardPage(-1)
                        event.accepted = true; break
                    case Qt.Key_PageDown:
                        keyboardPage(1); event.accepted = true; break
                    case Qt.Key_Home:
                        goToEarliestLoaded(); event.accepted = true; break
                    case Qt.Key_End:
                        goToLatest(); event.accepted = true; break
                    case Qt.Key_Space:
                        // Shift+Space pages up, Space pages down.
                        keyboardPage((event.modifiers & Qt.ShiftModifier) ? -1 : 1)
                        event.accepted = true; break
                    default:
                        event.accepted = false
                    }
                }
                // Clicking the timeline gives it keyboard focus without
                // stealing focus while typing.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: timeline.forceActiveFocus()
                }

                // TimelineScrollController drives wheel and keyboard motion;
                // QML applies each frame against the live bounds.
                Connections {
                    target: app.timelineScroll
                    function onWheelPositionChanged(y) {
                        var lo = timeline.wheelMinY()
                        // A glide can also reach the window's synthetic newest
                        // edge; give the rows back rather than settling at a
                        // false bottom.
                        if (y <= lo && timeline.rowWindowSkip > 0
                            && timeline.extendRowWindowAtNewEnd()) {
                            // The extension moved contentY and translated the
                            // motion, so this frame's y is stale. Drop it.
                            return
                        }
                        var hi = timeline.wheelMaxY()
                        var clamped = y < lo ? lo : (y > hi ? hi : y)
                        timeline.contentY = clamped
                        if (clamped !== y)
                            app.timelineScroll.notifyBoundReached(clamped)
                    }
                    function onWheelMotionSettled() {
                        timeline.updateStickAndPaginate()
                        // Refresh the anchor as soon as wheel motion ends, so a
                        // late height change cannot re-align to a position the
                        // reader has left.
                        timeline.captureViewAnchor()
                        scrollSettleTimer.restart()
                    }
                }

                // Save the settled position once input stops.
                Timer {
                    id: scrollSettleTimer
                    objectName: "scrollSettleTimer"
                    interval: 250
                    onTriggered: {
                        timeline.refreshMediaBand()
                        timeline.updateStickAndPaginate()
                        timeline.saveRoomPosition()
                        timeline.captureViewAnchor()
                        // The only place the row window is applied: never
                        // mid-gesture.
                        timeline.applyRowWindow()
                        // A completed gesture near the top re-arms the edge;
                        // otherwise backfill stops in a filtered room once the
                        // controller's strike budget is spent. The re-arm is
                        // unconditional within the band because
                        // checkNearTopEdge() owns the progress gate; a
                        // direction test here let up-then-down fetch and
                        // stranded a reader parked at the exact top.
                        if (!timeline.stickToBottom
                            && !app.pagination.reachedStart
                            && timeline.distanceFromTop()
                               <= timeline.nearTopEnterDistance)
                            timeline.nearTopArmed = true
                        // Single settle point: emit one diagnostic summary.
                        timeline.diagFlushGesture()
                    }
                }

                WheelHandler {
                    id: timelineWheelHandler
                    objectName: "timelineWheelHandler"
                    // Single pointer-wheel owner. Loading older history extends
                    // the far edge, so visible rows do not move under it.
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        // Timed only while LIGHTNING_SCROLL_TRACE is set.
                        var notchStartMs = timeline.scrollTrace ? Date.now() : 0
                        // Stop any autoscroll; a wheel notch also owns
                        // contentY.
                        root.stopAutoscroll()
                        // Also cancel a pending jump, or it would teleport the
                        // view once its target becomes measurable.
                        timeline.noteReaderTookControl()
                        // A positive delta on either axis is the older
                        // direction on this rotated view.
                        var towardsOlder = event.pixelDelta.y !== 0
                                ? event.pixelDelta.y > 0
                                : event.angleDelta.y > 0
                        // Extend the window before computing the motion, so the
                        // notch clamps against the extended geometry.
                        if (!towardsOlder && timeline.nearWindowNewEdge())
                            timeline.extendRowWindowAtNewEnd()
                        // Evaluated before dispatch; see wheelCanMove().
                        var canMove = timeline.wheelCanMove(towardsOlder)
                        var minY = timeline.wheelMinY()
                        var maxY = timeline.wheelMaxY()
                        // Content shorter than the viewport cannot scroll, so
                        // the near-top trigger never fires; once the fill
                        // budget is spent, a wheel toward older history is the
                        // request. User-initiated, so the controller re-arms
                        // its cap.
                        if (towardsOlder && !canMove && maxY <= minY + 0.5
                                && app.pagination && !app.pagination.reachedStart)
                            app.pagination.requestNearTop(true)
                        // A continuous source (touchpad on Wayland, phased
                        // scroll on macOS) is pixel input for the whole
                        // gesture, including frames with pixelDelta 0. Qt
                        // Wayland rounds each frame to whole pixels, carries
                        // the remainder, and still sends angleDelta; treating a
                        // px=0 frame as a notch made slow swipes jerk and
                        // travel farther than fast ones. A px=0 continuous
                        // frame moves nothing. Discriminate on `phase`, not
                        // device type: Qt Wayland can report a mouse wheel as
                        // TouchPad. Wheels, X11 and Windows touchpads are
                        // NoScrollPhase and keep the notch path.
                        var continuousSource = event.phase !== Qt.NoScrollPhase
                        if (event.pixelDelta.y !== 0
                                || (continuousSource && event.angleDelta.y !== 0)) {
                            timeline.cancelWheelMotion()
                            timeline.contentY = app.timelineScroll.pixelTargetY(
                                -event.pixelDelta.y, timeline.contentY,
                                minY, maxY)
                            timeline.updateStickAndPaginate(canMove)
                            if (towardsOlder && canMove)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0
                                   && !timeline.smoothScrollingEnabled) {
                            // Smooth scrolling off: land the notch at once,
                            // same distance. notchDistance() is the only
                            // stateless call on the controller; wheelTargetY()
                            // and pixelTargetY() mutate its shared motion
                            // state.
                            timeline.cancelWheelMotion()
                            var per = app.timelineScroll.notchDistance(
                                timeline.height)
                            // Not negated: wheelTargetY() negates internally
                            // and the smooth branch passes -angleDelta to
                            // cancel it, since upward is increasing contentY
                            // here. notchDistance() does no negation, so
                            // negating it would invert the wheel.
                            var jump = (event.angleDelta.y / 120.0) * per
                            timeline.contentY = Math.max(
                                minY, Math.min(maxY, timeline.contentY + jump))
                            timeline.updateStickAndPaginate(canMove)
                            if (event.angleDelta.y > 0 && canMove)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0) {
                            app.timelineScroll.wheelNotch(
                                -event.angleDelta.y, timeline.contentY,
                                minY, maxY, timeline.height)
                            timeline.updateStickAndPaginate(canMove)
                            if (event.angleDelta.y > 0 && canMove) {
                                timeline.stickToBottom = false
                                // Wheeling up mid-glide retires a pending
                                // follow-latest arrival.
                                timeline.followLatestOnArrival = false
                            }
                            timeline.diagNoteEvent(false)
                            scrollSettleTimer.restart()
                        }
                        timeline.diagNoteNotchCost(notchStartMs)
                        event.accepted = true
                    }
                }

                // A drag or flick owns contentY; cancel any glide. Only native
                // input reaches these.
                onDragStarted: { cancelWheelMotion(); noteReaderTookControl() }
                onFlickStarted: { cancelWheelMotion(); noteReaderTookControl() }

                Component.onDestruction: cancelWheelMotion()

                // Read state is decided by ReadReceiptCoordinator in C++. QML
                // reports only whether the timeline is on screen and following
                // the bottom.
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
                // Active-room notification suppression uses the same signals as
                // read receipts.
                Binding {
                    target: app
                    property: "activeRoomAtLatest"
                    value: timeline.visible && timeline.presentationReady
                           && timeline.Window.active === true
                           && timeline.stickToBottom
                }
                // The binding above is false until the view settles, while
                // opening a room delivers its recent history as live appends;
                // without this the room being read would notify for every
                // message it loads.
                Binding {
                    target: app
                    property: "activeRoomHydrating"
                    value: timeline.visible && !timeline.presentationReady
                           && timeline.Window.active === true
                }

                // Request another batch while the content cannot fill the
                // viewport (a short snapshot never scrolls, so a scroll trigger
                // alone would deadlock). The controller enforces budget,
                // no-progress stop and single-flight. Coalesced onto the next
                // turn: the pagination header changes contentHeight itself, and
                // dispatching directly caused a binding loop.
                property bool viewportFillCheckScheduled: false
                // The fill loop is self-healing, not only geometry-triggered.
                // Two ordinary things leave it with no trigger: request() drops
                // a dispatch while one is in flight, and a batch can insert
                // rows that render at zero height (routine activity while
                // hidden), so no geometry signal fires. With content shorter
                // than the viewport, every input path is then a no-op and the
                // room cannot be scrolled.
                //
                // This counter's subject is "the dispatch went nowhere", not
                // "the page brought nothing back"; a completed empty page is
                // counted by viewportFillEmptyPages instead.
                //
                // Bounded twice: this counter caps consecutive re-arms (the
                // controller's budget is not spent by a dropped dispatch), and
                // the controller's budget latches fillStopped (this counter
                // alone would not stop a backend returning invisible rows).
                // reachedStart/failed end it too.
                readonly property int maxViewportFillRetries: 8
                /// Pages that added rows but no visible height. Generous, since
                /// each advances the cursor through a collapsed run toward real
                /// messages, but bounded.
                // Each page is a round trip plus twenty instantiated rows on
                // every open, so this stays small (~240 hidden events). Past
                // it, scrolling up on the short content pulls the next page.
                readonly property int maxInvisibleFillRetries: 12
                // Hard cap on rows the fill may load, whatever their
                // visibility. A page that adds a little height counts as
                // progress and resets the invisible-page budget, so without
                // this a collapsed run growing one line per page could
                // instantiate hundreds of rows. Deeper history is left to the
                // reader's own scrolling.
                readonly property int maxViewportFillRows: 240
                /// Pages the backend completed that added no row and did not
                /// reach the start: history the timeline filter empties
                /// (MatrixRTC membership churn). Unlike a dispatch that went
                /// nowhere, the cursor advanced. Matches
                /// PaginationController::kMaxFilteredRunStrikes so neither side
                /// stops first. Affordable because an empty page instantiates no
                /// delegates and skips the completion settle.
                readonly property int maxEmptyFillPages: 60
                property int viewportFillInvisibleRetries: 0
                property int viewportFillEmptyPages: 0
                property int viewportFillRetries: 0
                /// contentHeight at the previous attempt, or -1. The budget is
                /// spent on attempts that did not make the content taller.
                property real viewportFillLastHeight: -1
                /// Loaded row count at the previous attempt, or -1.
                property int viewportFillLastRows: -1
                /// app.pagination.emptyFillPages at the previous attempt, or -1.
                /// The counter is monotonic within a room, so a change means a
                /// page actually completed.
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
                        // Decide nothing while a page is in flight: every
                        // geometry signal calls this, and calls between request
                        // and completion saw no growth only because the page
                        // had not arrived. The retry timer is re-armed so a
                        // page that moves no geometry still wakes this up.
                        if (app.pagination.busy) {
                            viewportFillRetryTimer.restart()
                            return
                        }
                        // Names which bound declined the fill, including the
                        // controller's.
                        function declineReason() {
                            if (app.pagination.reachedStart) return "reachedStart"
                            if (app.pagination.fillStopped) return "fillStopped"
                            if (app.pagination.failed) return "failed"
                            if ((app.timeline ? app.timeline.count : 0)
                                    >= maxViewportFillRows)
                                return "rowBudget"
                            // An empty timeline is not a reason to stop:
                            // maxViewportFillRetries counts pages that added no
                            // rows, exactly what filtered history produces.
                            // While empty, leave the decision to the
                            // controller's own terminators, which set
                            // fillStopped (checked at the top of this
                            // function).
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
                        // Two kinds of progress, only one visible. A page that
                        // added rows is progress even with no added pixels (a
                        // collapsed state run can hold contentHeight far below
                        // the viewport), and gets the generous bound; a page
                        // that added nothing gets the small one. Expanding an
                        // activity group must never be required to reach older
                        // messages. The request is gated by the check, so there
                        // is one request per completed page, not one per
                        // geometry signal.
                        var loadedRows = app.timeline ? app.timeline.count : 0
                        var emptyPages = app.pagination
                                ? app.pagination.emptyFillPages : 0
                        var grewHeight = viewportFillLastHeight >= 0
                                && contentHeight > viewportFillLastHeight + 1
                        var grewRows = viewportFillLastRows >= 0
                                && loadedRows > viewportFillLastRows
                        // The third kind: a page that completed and inserted
                        // nothing, detected by the controller's monotonic
                        // counter moving. Not the same as a dispatch that went
                        // nowhere.
                        var walkedFilteredHistory =
                                viewportFillLastEmptyPages >= 0
                                && emptyPages > viewportFillLastEmptyPages
                        if (grewHeight) {
                            // Visible history gained: nothing spent.
                            viewportFillRetries = 0
                            viewportFillInvisibleRetries = 0
                            viewportFillEmptyPages = 0
                        } else if (grewRows) {
                            // Invisible progress: keep going, but not forever.
                            viewportFillRetries = 0
                            viewportFillEmptyPages = 0
                            ++viewportFillInvisibleRetries
                        } else if (walkedFilteredHistory) {
                            // Filtered progress gets the larger budget (no
                            // delegates). It does not refund the invisible
                            // budget; each run bounds its own.
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
                // Any delegate height settling changes contentHeight. One
                // coalesced reaction: stay pinned to the newest event while
                // following, otherwise hold the anchor.
                onContentHeightChanged: {
                    // Any Column relayout after the reset reflects the new
                    // model's delegates.
                    presentationGeometryStale = false
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
                    refreshMediaBand()
                    maybeFillViewport()
                    recomputePresentationReady()
                    // Viewport resizes keep the reading position: pinned stays
                    // pinned, an anchored reader keeps the anchored message.
                    if (stickToBottom) {
                        if (count > 0)
                            Qt.callLater(scrollToEndDeferred)
                    } else {
                        maintainViewAnchorCoalesced()
                    }
                }

                // Prepend compensation is handled by the persistent view anchor
                // (maintainViewAnchor(), via onContentHeightChanged), like any
                // other height change above the reader. Do not reintroduce a
                // pagination-specific anchor.

                Connections {
                    target: app.pagination
                    function onPaginationCompleted(insertedCount, reachedStart,
                                                   willContinue) {
                        // Spend the approach's row budget on rows landing while
                        // the reader is in the near-top band. Measured before
                        // the early return so continuing batches are paid for;
                        // skipped at the live edge, where the viewport fill has
                        // its own cap.
                        if (insertedCount > 0 && !timeline.stickToBottom
                                && timeline.distanceFromTop()
                                   <= timeline.nearTopEnterDistance)
                            timeline.nearTopRowsThisApproach += insertedCount
                        if (insertedCount <= 0 || reachedStart || willContinue)
                            return
                        // A productive page moved the history edge: start a
                        // fresh ratchet so upward motion can prefetch the next
                        // page. No position write.
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance =
                                timeline.distanceFromTop()
                    }
                    function onTargetLocated(row, pixelOffset, highlight) {
                        // `highlight` distinguishes a reply jump (true) from a
                        // scroll-anchor restore (false). A gesture cancels
                        // every pending jump via noteReaderTookControl(); this
                        // also drops a restore that resolves just after the
                        // reader took over. Reply jumps issued after a gesture
                        // still land.
                        if (!highlight && timeline.readerControlledSinceReset) {
                            ++timeline.diagNavigationAbandoned
                            return
                        }
                        // Reply navigation takes control immediately.
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
                        Qt.callLater(function() {
                            // The target may be older than the paced-out rows
                            // or behind the row window.
                            timeline.releasePendingRows()
                            // See beginNavigationLanding(): landing in this
                            // turn was a silent no-op for targets not yet
                            // exposed.
                            timeline.beginNavigationLanding(row, pixelOffset,
                                                            highlight)
                        })
                    }
                    function onRestoreLatestRequested() {
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
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
                        // Capture the anchor right before the reflow and
                        // correct right after, on the same deferred turn,
                        // rather than waiting for onContentHeightChanged.
                        timeline.captureViewAnchor()
                        Qt.callLater(function() {
                            timeline.maintainViewAnchor()
                            timeline.maybeFillViewport()
                        })
                    }
                }

                // The one open row menu, handed over by
                // MessageDelegate.openContextMenu(). It is positioned once, so
                // any content movement closes it.
                property var openRowMenu: null
                onContentYChanged: {
                    if (openRowMenu) {
                        var stale = openRowMenu
                        openRowMenu = null
                        stale.close()
                    }
                    mediaBandSettle.restart()
                    // Unconditional: the activation range tracks programmatic
                    // moves too.
                    scheduleVisibleRowRange()
                    // React to user-driven movement only. A native wheel need
                    // not expose `moving`; the passive observer keeps the
                    // settle timer active.
                    if (!userScrollActive) return
                    stickToBottom = atBottomEdge()
                    // Backfill as the user approaches the top, edge-latched
                    // with hysteresis.
                    checkNearTopEdge(true)
                }
                onCountChanged: {
                    scheduleVisibleRowRange()
                    // A new event arrived or the timeline reset; follow the
                    // bottom.
                    recomputePresentationReady()
                    if (stickToBottom) Qt.callLater(scrollToEndDeferred)
                }
                onMovementEnded: {
                    // Scrolling settled: recompute at-bottom (return to bottom
                    // clears unread via the nearBottom binding).
                    refreshMediaBand()
                    stickToBottom = atBottomEdge()
                    saveRoomPosition()
                    captureViewAnchor()
                }
                Component.onCompleted: {
                    refreshMediaBand()
                    Qt.callLater(scrollToEndDeferred)
                    maybeFillViewport()
                    // The pane may be created while a room is already loaded.
                    if (app.currentRoomId !== "" && count > 0)
                        presentationGuard.restart()
                    recomputePresentationReady()
                }
                // A room switch or fresh snapshot opens at the bottom.
                Connections {
                    target: app.timeline
                    // Captured before the reset reaches the proxy: were there
                    // old rows whose delegates could linger? If not (first room
                    // of the session), arming the staleness gate would only
                    // delay the fast open.
                    function onModelAboutToBeReset() {
                        timeline.presentationResetHadRows = timeline.count > 0
                    }
                    function onModelReset() {
                        // Cancel wheel motion and autoscroll from the previous
                        // room.
                        timeline.cancelWheelMotion()
                        timeline.refreshMediaBand()
                        root.stopAutoscroll()
                        // Any reset discards the rows these surfaces came from,
                        // including a same-room jump-to-live trim that fires no
                        // switch cleanup.
                        root.closeRowAnchoredSurfaces()
                        timeline.stickToBottom = true
                        timeline.pinnedActionsKey = ""
                        timeline.emojiPickerOpen = false
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorOffset = 0
                        timeline.viewAnchorLastY = 0
                        // Re-arm the near-top edge for the new room.
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance = Infinity
                        timeline.nearTopRowsThisApproach = 0
                        timeline.nearTopRequestsThisApproach = 0
                        timeline.expandedStateGroups = ({})
                        // Fresh fill-retry budget for the new room.
                        timeline.viewportFillRetries = 0
                        timeline.viewportFillInvisibleRetries = 0
                        timeline.viewportFillLastHeight = -1
                        timeline.viewportFillLastRows = -1
                        viewportFillRetryTimer.stop()
                        // A pending landing belongs to the snapshot it was
                        // resolved against.
                        timeline.navigationPendingRow = -1
                        timeline.navigationPendingId = ""
                        navigationLandingTimer.stop()
                        // A fresh room's anchor restore is welcome.
                        timeline.readerControlledSinceReset = false
                        // Re-engage the gate. Recompute after this dispatch
                        // settles: the pagination controller's reset slot may
                        // run later, and reading its previous state here could
                        // open the gate on a one-item snapshot.
                        timeline.presentationReady = false
                        timeline.presentationResetPending = true
                        timeline.presentationGeometryStale =
                                timeline.presentationResetHadRows
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

                // Pagination trigger at the top; duplicate and reached-start
                // suppression live in the controller. Not edge-latched through
                // checkNearTopEdge(): that latch returns early while
                // stickToBottom, and a freshly opened room too short to scroll
                // relies on this passive edge for its first fill. It is bounded
                // anyway: atYBeginning only fires on a real false->true
                // transition, userInitiated=false cannot reset the controller's
                // empty-strike counter, request() is single-flight, and a
                // productive page ends the run.
                onAtYEndChanged: {
                    if (atYEnd)
                        maybeRequestNearTop(false)
                }

                // The loading/failure indicator is a top overlay
                // (paginationHeader), not list content, so toggling it never
                // changes timeline geometry.

                // No attached ScrollBar or empty-state label here: children of
                // the rotated Flickable render mirrored or upside down. Both
                // are unrotated siblings.
            }

            // Timeline scrollbar, outside the rotated Flickable. The visual top
            // is wheelMaxY (oldest) and the bottom wheelMinY (newest); the
            // mapping inverts both ways so dragging works.
            AppScrollBar {
                id: timelineScrollBar
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
                onPositionChanged: {
                    // Only follow the handle while the user holds it.
                    if (!pressed)
                        return
                    var frac = (1 - size) > 0 ? position / (1 - size) : 0
                    timeline.cancelWheelMotion()
                    // Dragging the handle retires an unlanded jump, like the
                    // wheel.
                    timeline.noteReaderTookControl()
                    timeline.contentY = timeline.wheelMaxY() - frac * span
                    timeline.updateStickAndPaginate()
                }
            }

            Binding {
                target: timelineScrollBar
                property: "position"
                // Yields while dragged.
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

            // Pagination loading/failure indicator: a top overlay, never list
            // content, so Loading cannot change contentHeight or flip
            // atYBeginning. Height tracks the PaginationController state
            // directly (no local mirror). objectName "paginationHeader" is used
            // by TimelinePaneQmlTest.
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
                        // An inline link (AppTheme.link, not accent). An
                        // AbstractButton rather than a Label with a MouseArea,
                        // so it is focusable and accessible.
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
                            // Cursor only; accepting buttons would swallow the
                            // clicks.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }
                }
            }

            // Home surface when nothing is selected, over the hidden timeline
            // (which stays present for tests and to resume instantly). A real
            // selected Space gets its management surface; pseudo-spaces keep
            // Home.
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
            }

            // Middle-click autoscroll. A sibling of the rotated Flickable (a
            // child would get mirrored coordinates); drives contentY through
            // the same bounds as the wheel, `inverted` because newer messages
            // are at lower contentY.
            MiddleClickScroller {
                id: middleClickScroller
                objectName: "timelineMiddleClickScroller"
                anchors.fill: parent
                z: 2
                visible: app.currentRoomId !== "" && timeline.presentationReady
                view: timeline
                inverted: true
                minYFunc: function() { return timeline.wheelMinY() }
                maxYFunc: function() { return timeline.wheelMaxY() }
                // Cancel a wheel glide once at gesture start. stickToBottom is
                // derived from the real position, so scrolling back down
                // re-arms follow-latest.
                onActiveChanged: if (active) {
                    timeline.cancelWheelMotion()
                    // Starting an autoscroll is the reader taking the view.
                    timeline.noteReaderTookControl()
                }
                onScrolled: {
                    timeline.updateStickAndPaginate()
                    // The scroller writes contentY directly, leaving
                    // Flickable.moving false, so without this userScrollActive
                    // stays false for the whole gesture: no anchor capture, and
                    // the idle branch of maintainViewAnchor() would restore the
                    // pre-gesture position on the next height change.
                    // Restarting the settle timer gives autoscroll the same
                    // settle path as the wheel.
                    scrollSettleTimer.restart()
                }
            }

            // Space Home: never an ordinary room timeline/composer.
            Loader {
                objectName: "spaceHomeLoader"
                anchors.fill: parent
                active: parent.spaceViewActive
                visible: active
                sourceComponent: spaceHomeComponent
            }

            // Room-loading surface shown while the presentation gate holds the
            // timeline back: one quiet loading row, no partial rows.
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

            // Empty room: a start-of-conversation block with the room's
            // identity and actions. An unrotated sibling of the timeline.
            // `count === 0 && presentationReady` means nothing is loaded, not
            // that the room has no history, so the wording stays modest.
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
                        mxc: root.currentRoom.avatarUrl || ""
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
                    // Only when the SDK says the room is encrypted.
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: AppTheme.spacingXS
                        visible: root.currentRoom.encrypted === true
                        Icon {
                            // Neutral, not green: a room-state fact, not a
                            // trust claim.
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
                            // Fails closed: the roster controller may point at
                            // another room.
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

            // Jump to first unread, at the top (mirror of the jump-to-latest
            // pill). Offered only when the marker row is loaded, so the jump is
            // exact; deeper unread is reached via goToFirstUnread() from the
            // room list.
            AbstractButton {
                id: jumpToFirstUnreadButton
                objectName: "jumpToFirstUnreadButton"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingM
                visible: app.currentRoomId !== "" && app.timeline
                         && app.timeline.firstUnreadRow >= 0
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
                    // The unread colour, matching the divider and badges;
                    // accent belongs to jump-to-latest.
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
                        anchors.verticalCenter: parent.verticalCenter
                        name: "expand_less"
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

            // Jump-to-latest pill with an optional new-message count. An
            // AbstractButton so click() and visible drive the scroll tests.
            AbstractButton {
                id: jumpToLatestButton
                objectName: "jumpToLatestButton"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: AppTheme.spacingM + 12
                anchors.bottomMargin: AppTheme.spacingM + 8
                // Hidden as soon as the trip home starts: stickToBottom only
                // flips on arrival on the glide path.
                visible: app.currentRoomId !== "" && !timeline.stickToBottom
                         && !timeline.followLatestOnArrival
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
                // Shared with the End key.
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
        }

        // Typing indicator: a constant-height slot, so appearing and
        // disappearing never resizes the timeline viewport. Only the label's
        // opacity changes.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: app.currentRoomId !== ""
                ? Math.max(typingLabel.implicitHeight, typingMetrics.height) + 6
                : 0
            visible: app.currentRoomId !== ""
            color: AppTheme.background
            FontMetrics {
                id: typingMetrics
                font.family: AppTheme.uiFont
                font.italic: true
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
            Label {
                // Untrusted text: never markup.
                textFormat: Text.PlainText
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                opacity: text.length > 0 ? 1 : 0
                // Keep the empty state out of the accessibility tree.
                Accessible.ignored: text.length === 0
                elide: Text.ElideRight
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.italic: true
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }

        // Save As feedback (auto-clears). Zero layout height: the banner
        // overflows upward over the timeline instead of pushing it.
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

        // Selection bar for multi-message forwarding, above the composer.
        // Escape and Cancel both leave the mode.
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
                // Honour the autoscroll and screen gates here too (same key,
                // same window).
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
            // Visible during a call: the timeline stays on screen.
            visible: app.currentRoomId !== ""
        }
    }

    // Thread side panel
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
        // The thread panel is 340px wide.
        Layout.preferredWidth: root.width >= 660 ? 340 : root.width
        Layout.fillWidth: root.threadSurfaceOpen && root.width < 660
        onCloseRequested: {
            // Closing the thread collapses the right side; Room Information is
            // not restored implicitly.
            root.closeThreadSurface()
        }
        openImage: function(mediaKey, httpUrl) {
            imageViewer.openFor(mediaKey || "", httpUrl)
        }
        saveMedia: function(mediaKey, filename) {
            if (!mediaKey || mediaKey.length === 0) return
            saveMediaDialog.pendingMediaKey = mediaKey
            saveMediaDialog.currentFile = root.suggestedSaveUrl(filename)
            saveMediaDialog.open()
        }
    }

    // Room Information side panel. The divider is also the resize grab. Width
    // persists in SettingsManager::sidePanelWidth, which clamps it.
    Item {
        id: infoResizer
        objectName: "roomInfoResizeHandle"
        visible: root.infoOpen
        Layout.fillHeight: true
        // One pixel, not a band: a wider band has no colour that is right
        // beside panes that change colour along their height. The grab area
        // comes from the handlers' `margin` instead (same as MainScreen's
        // SplitView handle).
        implicitWidth: 1

        Rectangle {
            anchors.fill: parent
            color: infoDrag.active ? AppTheme.accent
                 : infoHover.hovered ? AppTheme.borderStrong : AppTheme.border
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        HoverHandler {
            id: infoHover
            // Grab area without painted width.
            margin: 4
            cursorShape: Qt.SplitHCursor
        }
        DragHandler {
            id: infoDrag
            margin: 4
            target: null
            yAxis.enabled: false
            // Width at grab: translation is cumulative from the press.
            property int startWidth: 0
            onActiveChanged: {
                if (active)
                    startWidth = app.settings.sidePanelWidth
            }
            onTranslationChanged: {
                if (!active)
                    return
                // The panel is on the right, so dragging left widens it.
                app.settings.sidePanelWidth =
                    Math.round(startWidth - translation.x)
            }
        }
    }
    RoomInfoPanel {
        id: infoPanel
        objectName: "roomInfoPanel"
        Layout.fillHeight: true
        // Capped by the space actually available, not only the stored width,
        // which can outlive a wider window; otherwise the row overflows off the
        // right edge. 420 is the conversation's floor; the panel's is 260, and
        // the >= 700 gate keeps them from fighting.
        Layout.preferredWidth: root.infoOpen
            ? Math.max(260, Math.min(app.settings.sidePanelWidth,
                                     root.width - 420))
            : 0
        // Collapse at narrow widths instead of crushing the chat.
        visible: root.infoOpen && root.width >= 700
        onCloseRequested: root.infoOpen = false
        onOpenImagesRequested: (entries, index) =>
            imageViewer.openAt(entries, index)
        onSaveMediaRequested: (mediaKey, filename) => {
            saveMediaDialog.pendingMediaKey = mediaKey
            saveMediaDialog.currentFile = root.suggestedSaveUrl(filename)
            saveMediaDialog.open()
        }
        // Pins often lie outside the loaded window; reuse
        // PaginationController::jumpToEvent, the single navigation path.
        onJumpToEventRequested: (eventId) => {
            if (eventId !== "")
                app.pagination.jumpToEvent(eventId)
        }
        onExportRoomRequested: root.openExportRoom()
    }

    // Room message search side panel
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

    // Space Home: the management surface for a selected Space. Hierarchy data
    // is SpaceManager state; "Add existing room" sends the real state event and
    // the list updates when sync confirms.
    Component {
        id: spaceHomeComponent
        Rectangle {
            id: spaceHome
            objectName: "spaceHomePane"
            // The pane ground, not the raised card tone; cards inside use
            // surface/cardElevated.
            color: AppTheme.background

            readonly property string spaceId:
                app.spaces ? app.spaces.activeSpaceId : ""
            property var info: ({})
            // Assigned by rebuildLobby(), never bound: its inputs come from a
            // C++ call, and a binding through a function call does not track
            // what it reads.
            property var lobbySections: []
            // Joined subspaces with a lobby section. The SDK's /hierarchy
            // listing is one level deep, so each is asked separately.
            property var lobbySubspaceIds: []
            // /hierarchy rows by space id: the only source of topics for
            // unjoined rooms, and of member counts and suggested flags.
            property var hierarchyBySpace: ({})
            // /hierarchy has not answered yet: show "Loading rooms…", not "No
            // rooms yet".
            property bool homeLoading: false
            property var loadingIds: ({})
            // Unjoined /hierarchy children across this Space and its subspaces.
            // Used by onSpaceJoined to tell this lobby's joins from others.
            property var unjoinedChildren: []
            property string addNotice: ""
            property bool settingsOpen: false
            // A Space is a Matrix room, so it has a real member list (the same
            // roster Room Information reads, already pointed at this Space).
            property bool peopleOpen: false

            InvitePeopleDialog {
                id: spaceInviteDialog
                parent: Overlay.overlay
            }

            Connections {
                target: app.discovery
                // Joining a sub-space from the offers below drills into it. The
                // signal is global (the Discover dialog emits it too), so only
                // drill for one of this Space's own offers.
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
                lobbySubspaceIds = app.spaces
                                 ? app.spaces.lobbySubspaceIds(spaceId) : []
                // A subspace that appeared since the Home opened has not been
                // asked about.
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
            // RoomDiscoveryController is single-flight per space, so these do
            // not drop one another.
            function requestHierarchy() {
                if (spaceId === "" || !app.discovery.supported)
                    return
                app.discovery.refreshSpaceChildren(spaceId)
                for (var i = 0; i < lobbySubspaceIds.length; ++i)
                    app.discovery.refreshSpaceChildren(lobbySubspaceIds[i])
            }
            // After a join or knock, re-ask only the space(s) listing it.
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
            // Coalesced to one lobbySections() call per event-loop turn.
            function scheduleRebuild() { Qt.callLater(spaceHome.rebuildLobby) }
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
                selectedChildIds = ({})
                childFilter = ""
                removeChildConfirm.close()
                removeChildConfirm.roomIds = []
                spaceLobby.closeMenus()
                refresh()
                requestHierarchy()
                // Point RoomInfoController at the space while its Home is
                // shown: the Invite gate and settings card read it. A Space
                // never becomes app.currentRoomId, so nothing contends.
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
                        Qt.callLater(spaceHome.refreshUnjoined)
                }
                // Re-read the hierarchy so the offer disappears; the joined
                // list updates via sync.
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
                    // The cached /hierarchy still lists it: re-ask.
                    suggestRefreshCoalesce.restart()
                }
                function onChildSuggestedFinished(spaceId, roomId,
                                                  suggested, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    if (!ok)
                        spaceHome.addNotice =
                            qsTr("The suggested flag could not be changed "
                                 + "— you may not have permission.")
                    // The suggested flag lives on /hierarchy rows, so refetch
                    // (never applied optimistically). Coalesced:
                    // refreshSpaceChildren is single-flight with no queue.
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

                // Space banner, under the event type Sable writes (see
                // rust/src/banner.rs). Shown only when there is one or this
                // account could add one.
                Rectangle {
                    id: spaceBannerCard
                    objectName: "spaceHomeBanner"
                    // Full-bleed, with the height taken from the image's own
                    // aspect ratio so the whole picture shows without cropping;
                    // bounded so an extreme image cannot take over or vanish.
                    // Outside the centred 880px column, whose width would
                    // change the shape.
                    readonly property real bannerAspect:
                        (spaceBannerImage.status === Image.Ready
                         && spaceBannerImage.implicitHeight > 0)
                        ? (spaceBannerImage.implicitWidth
                           / spaceBannerImage.implicitHeight)
                        : 0
                    // Cropped by default: a fixed strip, as Sable renders it.
                    // Expanded uses the picture's aspect ratio. App-wide and
                    // remembered.
                    readonly property bool expanded:
                        app.settings.spaceBannerExpanded
                    x: 0
                    width: spaceScroll.width
                    height: Math.round(expanded && bannerAspect > 0
                        ? Math.max(120, Math.min(420, width / bannerAspect))
                        : 190)
                    clip: true
                    // Hidden when the user turned banners off.
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
                        // Same revision dependency as bannerMxc: false until
                        // the room answers, so the control is never offered on
                        // a guess.
                        var _dep = app.banners.revision
                        return app.banners.canSetRoomBanner(
                            spaceHome.spaceId)
                    }

                    // Asked once per Space per session; the write path re-asks
                    // itself.
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

                    // Empty state: theme tones only.
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
                        // Expanded: the box already has the picture's shape, so
                        // Fit shows all of it. Cropped: Crop fills the fixed
                        // strip.
                        fillMode: spaceBannerCard.expanded
                                  ? Image.PreserveAspectFit
                                  : Image.PreserveAspectCrop
                        // Bounded decode; scaling preserves the aspect ratio
                        // read above.
                        sourceSize.width: 1600
                        asynchronous: true
                        visible: status === Image.Ready
                        // Under a playing animation this first frame would
                        // show through its transparent pixels.
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
                    // An animated banner plays over the still one, from the
                    // same bytes.
                    BannerMotion {
                        id: spaceBannerMotion
                        objectName: "spaceBannerMotion"
                        anchors.fill: parent
                        mxc: spaceBannerImage.mxc
                        stillReady: spaceBannerImage.status === Image.Ready
                        fillMode: spaceBannerImage.fillMode
                    }
                    // A wash under the controls keeps their contrast on any
                    // image.
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

                    FileDialog {
                        id: spaceBannerDialog
                        title: qsTr("Choose a banner image")
                        fileMode: FileDialog.OpenFile
                        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp)") ]
                        // The crop dialog decides what is uploaded and refuses
                        // anything but the five raster formats before
                        // rendering.
                        onAccepted: spaceBannerCrop.openFor(selectedFile)
                    }
                    ImageCropDialog {
                        id: spaceBannerCrop
                        role: "banner"
                        // Pass the URL as-is; stripping "file://" here breaks
                        // Windows paths.
                        onCropped: function (file) {
                            app.banners.setRoomBanner(
                                spaceHome.spaceId, file.toString())
                        }
                    }

                    // All banner controls in one cluster on a scrim, as icons
                    // with tooltips: a control drawn on an arbitrary picture
                    // needs its own background. Uses the media-chrome tokens
                    // the GIF and size pills use.
                    Rectangle {
                        objectName: "spaceBannerControls"
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: AppTheme.spacing8
                        radius: height / 2
                        // Media-chrome token: this reads on top of an arbitrary
                        // picture.
                        color: AppTheme.scrimSurface
                        border.width: 1
                        border.color: AppTheme.overlayScrim
                        readonly property int pad: AppTheme.spacing4
                        implicitWidth: bannerControlRow.implicitWidth + pad * 2
                        implicitHeight: bannerControlRow.implicitHeight + pad * 2
                        // Visibility from the conditions, never from the
                        // children's `visible`: visible is effective
                        // visibility, so while the pill is hidden every child
                        // reports false and a sum of them can never turn it
                        // back on.
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
                                // Washed danger tone from the media chrome;
                                // AppTheme.danger is tuned for theme surfaces,
                                // not photographs.
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

                    // A refusal is reported in place; nothing was applied
                    // optimistically.
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
                    // Measured against the Flickable, not `parent`: a Flickable
                    // reparents children to contentItem, which mis-centred the
                    // column.
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
                            // Opening a Space is intent: probe any format.
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
                            Label {
                                visible: (spaceHome.info.topic || "").length > 0
                                text: spaceHome.info.topic || ""
                                // Unsanitized server text; never AutoText.
                                textFormat: Text.PlainText
                                color: AppTheme.textSecondary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                wrapMode: Text.WordWrap
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                maximumLineCount: 3
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var c = spaceHome.info.childCount || 0
                                    var u = spaceHome.info.unreadTotal || 0
                                    var line = qsTr("%n room(s)",
                                                    "rooms inside a Space", c)
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

                    // A Flow, not a RowLayout: which buttons are present
                    // depends on permissions, and a RowLayout does not wrap, so
                    // a narrow pane cut buttons off. A Flow is already
                    // left-aligned, so there is no fillWidth spacer.
                    Flow {
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
                            objectName: "spaceInviteButton"
                            // Same invite path and server-side permission gate
                            // as a room; shown only when the roster says we may
                            // invite.
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
                            onClicked: {
                                addRoomPopup.query = ""
                                addRoomPopup.refresh()
                                addRoomPopup.open()
                            }
                        }
                        AppButton {
                            objectName: "spacePeopleButton"
                            // Only when the roster is this Space's;
                            // app.roomInfo may still point at a room.
                            visible: app.roomInfo
                                     && app.roomInfo.roomId === spaceHome.spaceId
                            text: spaceHome.peopleOpen
                                  ? qsTr("Hide people")
                                  : qsTr("People (%1)").arg(
                                        (app.roomInfo.members || []).length)
                            onClicked: spaceHome.peopleOpen = !spaceHome.peopleOpen
                        }
                        AppButton {
                            objectName: "spaceSettingsButton"
                            text: spaceHome.settingsOpen
                                  ? qsTr("Hide settings") : qsTr("Space settings")
                            onClicked:
                                spaceHome.settingsOpen = !spaceHome.settingsOpen
                        }
                        // Bring back a banner hidden from its own corner, so
                        // hiding is not one-way.
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
                        visible: spaceHome.addNotice.length > 0
                        text: spaceHome.addNotice
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    // Space members, from the roster Room Information reads.
                    Rectangle {
                        objectName: "spacePeopleCard"
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
                                    // Bounded: the Flow is not virtualized.
                                    model: (app.roomInfo.members || []).slice(0, 60)
                                    delegate: Rectangle {
                                        id: spaceMemberChip
                                        required property var modelData
                                        radius: AppTheme.radiusPill
                                        color: chipHover.hovered ? AppTheme.hover
                                                                 : AppTheme.surface
                                        border.color: AppTheme.border
                                        border.width: 1
                                        implicitWidth: Math.min(
                                            chipRow.implicitWidth + AppTheme.spacing12, 240)
                                        implicitHeight: 34
                                        HoverHandler {
                                            id: chipHover
                                            cursorShape: Qt.PointingHandCursor
                                        }
                                        TapHandler {
                                            onTapped: senderProfilePopover.openFor(
                                                          spaceMemberChip.modelData)
                                        }
                                        RowLayout {
                                            id: chipRow
                                            anchors.fill: parent
                                            anchors.leftMargin: AppTheme.spacing4
                                            anchors.rightMargin: AppTheme.spacing10
                                            spacing: AppTheme.spacing6
                                            Avatar {
                                                size: 26
                                                name: spaceMemberChip.modelData.displayName
                                                      || spaceMemberChip.modelData.userId
                                                mxc: spaceMemberChip.modelData.avatarUrl || ""
                                                colorKey: spaceMemberChip.modelData.userId
                                                circle: true
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: spaceMemberChip.modelData.displayName
                                                      || spaceMemberChip.modelData.userId
                                                textFormat: Text.PlainText
                                                color: AppTheme.textPrimary
                                                font.pixelSize: AppTheme.textMeta
                                                elide: Label.ElideRight
                                            }
                                        }
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                visible: (app.roomInfo.members || []).length > 60
                                // Disclose the cap rather than silently
                                // truncating.
                                text: qsTr("Showing the first 60 of %1.")
                                      .arg((app.roomInfo.members || []).length)
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                    }

                    // Space settings, through the same permission-gated
                    // room-edit backend.
                    Rectangle {
                        visible: spaceHome.settingsOpen
                        Layout.fillWidth: true
                        radius: AppTheme.radiusMd
                        color: AppTheme.cardElevated
                        border.color: AppTheme.border
                        border.width: 1
                        implicitHeight: settingsCol.implicitHeight
                                        + AppTheme.spacing16 * 2
                        // roomInfo is bound to the space for the Space Home's
                        // lifetime (see Component.onCompleted below).
                        FileDialog {
                            id: spaceAvatarDialog
                            title: qsTr("Choose Space avatar")
                            fileMode: FileDialog.OpenFile
                            nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
                            onAccepted: spaceAvatarCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceAvatarCrop
                            role: "avatar"
                            // m.room.avatar through the same backend as a
                            // room's avatar.
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
                                    // canEditAvatar is the room's real required
                                    // level for m.room.avatar, from the member
                                    // snapshot.
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

                    // The Space's own rooms, then a collapsible section per
                    // subspace, each row with its topic. Presentation lives in
                    // SpaceLobby.qml and sections are built by
                    // SpaceManager::lobbySections(); this block feeds it and
                    // routes its requests to app calls.
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
                        onOpenRoomRequested: (roomId) => app.openRoom(roomId)
                        // A joined sub-space drills into its own Home.
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
                }
            }

            // Child-removal confirmation. Removes the hierarchy relation only,
            // never the room.
            Popup {
                id: removeChildConfirm
                // Driven by the list's selection: one confirm for N rooms.
                property var roomIds: []
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                // Storm surface plus the shared modal scrim, like other
                // confirms.
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

            // Leave confirmation. Leaving a Space never touches its rooms.
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

            // Add-existing-room picker: joined non-Space rooms, filtered, with
            // existing children marked and un-addable.
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

    // Files dragged anywhere over the chat queue as composer attachments.
    // DropArea only consumes drag events.
    DropArea {
        id: chatDropArea
        // Never cover the thread surface: ThreadPanel has its own DropArea, and
        // thread attachments must use the thread send path. Stops 340px short
        // beside a thread panel; zero width in the full-width thread layout
        // (<660).
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
        // Mirrors the drop area's thread-excluding geometry.
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
