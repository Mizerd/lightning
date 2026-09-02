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
    property bool searchOpen: false
    // v0.6.0: the thread side surface is open when a thread panel OR the
    // room's Threads list view is showing.
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen
    /// The call stage owns this column: a live MatrixRTC call in THIS room.
    ///
    /// ONE condition, read by the stage's Loader and by everything the stage
    /// replaces, because the two must never disagree. They did: the stage was
    /// documented as REPLACING the timeline and was only ever ADDED beside it,
    /// and both are `Layout.fillHeight` — so a ColumnLayout split the column
    /// between them and the stage got roughly half. That is what made the
    /// call UI look broken the moment it appeared: the participant spotlight
    /// was squeezed to a ~45px strip, its avatar and controls piled onto each
    /// other, and message rows kept showing underneath the control dock.
    readonly property bool callStageOwnsColumn:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId

    // ── The call panel's share of the column ─────────────────────────────
    //
    // Discord's DM call is a panel at the TOP of the channel column, auto-
    // sized and user-resizable, with the message list scrolling independently
    // beneath it. No Discord documentation states its default height — the
    // only figure anywhere is a user saying it takes "half the vertical
    // height", which is an estimate — so these are Lightning's numbers, not
    // measurements of Discord.
    //
    //   * 40 % of the column for a voice-only call, 70 % once anything is
    //     sending video, clamped to [220 px, 75 % of the column].
    //   * A drag STORES the user's height for the session. It does not
    //     disable the auto-grow: video appearing gives a one-shot nudge up
    //     (see onCallPanelHasVideoChanged) which the user may immediately
    //     drag back, and that drag sticks. Discord's manual resize latches
    //     the auto-size off permanently, which is reported there as a bug
    //     across three machines — worth not copying.
    //   * Collapsed, the panel is a one-line strip and the timeline gets the
    //     rest of the column back.
    //
    // Session-scoped, deliberately: persisting it needs a SettingsManager key
    // and that file is not this round's to change. Noted as a follow-up.
    property bool callPanelCollapsed: false
    /// < 0 means "no user preference yet — follow the automatic answer".
    property real callPanelUserHeight: -1
    /// The height the current drag started from. A DragHandler reports a
    /// translation, not a position, so the gesture needs its own origin.
    property real callPanelDragBase: 0
    /// The collapsed strip: one line of avatars, speaking rings and compact
    /// controls. 64 px because that is what the compact control row plus the
    /// panel's own margins measure — a smaller number clips the controls,
    /// which is worse than a slightly taller strip.
    readonly property real callPanelCollapsedHeight: 64
    /// True once anything in this call is sending video. Read off the stage
    /// rather than recomputed: the stage already owns that derivation and two
    /// copies of it would drift.
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

    /// The floor a panel SHOWING VIDEO gets, asked of the stage itself.
    ///
    /// The stage knows what it spends before a picture starts — its header,
    /// its dock, its own margins — and this pane does not. Reading the number
    /// rather than restating it is the same rule the `callPanelHasVideo`
    /// derivation above already follows; a second copy here would drift the
    /// first time one of those bands changes. Falls back to the old flat
    /// number while the stage is not loaded.
    readonly property real callPanelVideoFloor:
        callStageHost.active && callStageHost.item
        ? callStageHost.item.minimumUsefulHeight : 220

    function clampCallPanelHeight(value) {
        // The floor YIELDS to a small column. It exists to keep the call
        // usable, not to win an argument with the window: a floor taller than
        // the pane leaves no timeline at all, which is the thing this whole
        // change was made to stop.
        //
        // A PANEL SHOWING VIDEO ASKS FOR MORE, and that is the fix for
        // "when screen share is on it's too squishable, the UI breaks then".
        // At 45% of a short pane the stage was getting its header and its
        // dock and about ten pixels of picture, which is not a smaller
        // version of this surface — it is a broken one: the share collapsed
        // to a sliver and the spotlight's own overlay controls drew across
        // its top edge. A voice-only panel is unaffected; it has no picture
        // to protect and 45% is genuinely enough for it.
        //
        // Still a MINIMUM WITH A CEILING, not a demand: the `Math.min`
        // against the pane keeps a very short window from losing its timeline
        // entirely, and 0.6 rather than 0.45 is what buys the picture back.
        var floor = root.callPanelHasVideo
            ? Math.min(root.callPanelVideoFloor,
                       Math.max(64, root.height * 0.6))
            : Math.min(220, Math.max(64, root.height * 0.45))
        var ceiling = Math.max(floor, root.height * 0.75)
        return Math.max(floor, Math.min(value, ceiling))
    }

    // The one-shot grow. A share or a camera arriving is the moment the panel
    // needs to be bigger; after that the user's own height wins again, which
    // is why this writes the stored value once instead of becoming a floor
    // under it.
    onCallPanelHasVideoChanged: {
        if (!root.callPanelHasVideo || root.callPanelUserHeight < 0)
            return
        var target = clampCallPanelHeight(root.height * 0.70)
        if (root.callPanelUserHeight < target)
            root.callPanelUserHeight = target
    }

    // A call ending must not leave the NEXT call collapsed: the collapse is a
    // state of this call, and there is no control on a collapsed strip that
    // belongs to a call that is over.
    onCallStageOwnsColumnChanged: {
        if (!root.callStageOwnsColumn)
            root.callPanelCollapsed = false
    }
    // The authoritative right-panel state. Exactly one surface is open:
    // the thread surface (controller-owned state) wins, then the info/member
    // panel, else none. All open/close paths flow through the two underlying
    // states and their exclusivity handlers below.
    readonly property string rightPanelState:
        threadSurfaceOpen ? "thread"
        : (searchOpen ? "search" : (infoOpen ? "info" : "none"))

    function refreshCurrentRoom() {
        currentRoom = app.currentRoomId === ""
                      ? ({})
                      : app.roomList.findRoom(app.currentRoomId)
    }

    // The REVERSE half of the middle-click autoscroll interlock. The
    // scroller already cancels an in-flight wheel glide when its gesture
    // starts; without this the other direction was missing entirely, so a
    // wheel notch, a keyboard page, Jump to latest or a reply jump would
    // write contentY on alternate frames with a gesture that was still
    // running. ONE helper, named at every cancellation site, so the set
    // cannot drift the way the scroller's own did.
    //
    // Deliberately NOT folded into timeline.cancelWheelMotion(): the
    // scroller calls that itself when its gesture starts, and stopping the
    // gesture from inside it would end every autoscroll on its first frame.
    function stopAutoscroll() {
        if (middleClickScroller.active)
            middleClickScroller.stop()
    }

    // The right-side region is mutually exclusive: member/info panel OR
    // thread panel, never both layered. Exclusion lives on the two state
    // properties themselves so every open path — buttons, chips,
    // notifications, tests — flows through the same mechanism.
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

    // Header pin shortcut: opens the same side panel directly on the
    // Pinned section (mirrors toggleMemberPanel).
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
            if (app.currentScreen === 2) {
                root.infoOpen = false
                root.searchOpen = false
            }
        }
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            // A find session belongs to the room it was opened in — the
            // history-mode results included (roomId-scoped server search).
            if (root.findOpen) {
                root.findOpen = false
                root.findHistoryMode = false
                app.messageSearch.query = ""
                findField.text = ""
            }
            // Old-room wheel motion must not continue into the new room —
            // and neither may a middle-click autoscroll gesture, which
            // writes contentY directly and had no room-switch exit at all.
            timeline.cancelWheelMotion()
            root.stopAutoscroll()
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
            root.searchOpen = false
        }
    }

    // v0.6.1: find in loaded messages (this room's currently loaded
    // timeline; never a persistent index). v0.7.x adds an explicit History
    // segment — an honest server /search of this room — in unencrypted
    // rooms only; the two modes never mix results.
    property bool findOpen: false
    // v0.7.x: the find bar's second mode — server-side history search of
    // THIS room. Unavailable in encrypted rooms (the server cannot search
    // ciphertext; the loaded-messages find remains the only search there).
    property bool findHistoryMode: false
    // Review H1: the offer needs an AFFIRMATIVE unencrypted state — a room
    // whose encryption state has not synced yet gets no History segment.
    readonly property bool findHistoryAvailable:
        app.messageSearch.supported
        && root.currentRoom.encryptionKnown === true
        && root.currentRoom.encrypted !== true

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
        // The match may be an older row the proxy is still pacing out. Take
        // the whole backlog before resolving it, or the jump resolves to
        // "no such row" and the search silently does nothing.
        timeline.releasePendingRows()
        var row = timeline.viewRowForStableId(eventId)
        if (row < 0) return
        timeline.cancelWheelMotion()
        root.stopAutoscroll()
        timeline.stickToBottom = false
        timeline.positionViewAtViewRow(row, true)
    }
    Shortcut {
        // Was StandardKey.Find, which is Ctrl+F on Linux and Windows. The
        // registry's default is the same key, spelled explicitly so it can be
        // SHOWN and REBOUND — a StandardKey has no stable text to display and
        // cannot be overridden.
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.find")]
        }
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
        // An ACTIVE autoscroll owns Escape (MiddleClickScroller declares its
        // own shortcut for it). Two enabled Shortcuts on one sequence make Qt
        // report an ambiguous overload and fire NEITHER, so the exclusion has
        // to be explicit here rather than relying on ordering.
        enabled: !timeline.emojiPickerOpen && !middleClickScroller.active
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
    // Read-state on focus change is handled by the ListView's own
    // maybeMarkRead() gate (see the timeline below).
    Component.onCompleted: refreshCurrentRoom()

    // v0.5.9: in-app image viewer + explicit Save As for file attachments.
    ImageViewerOverlay {
        id: imageViewer
        onOpened: timeline.claimTransientInteraction("viewer")
        onClosed: timeline.releaseTransientInteraction("viewer")
    }

    // v0.7: ONE reaction picker and ONE sender-profile popover for the whole
    // timeline (previously every message row eagerly built its own picker
    // popup — dozens of live instances per screen). The target event id is
    // snapshotted at open; a room or account switch closes both.
    // 2026-08-18 tester report #2: clicking the read-by chips lists the
    // readers. ONE shared popover (sharedReactionPicker precedent) —
    // rows hand it plain data, never object references. Honesty rule:
    // the bridge delivers the newest 16 readers with a truthful
    // uncapped total, so beyond 16 the list ends with "+N more" and
    // never fabricates names.
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
        // The chips sit at a message's BOTTOM edge — open upward, so a
        // card that grows after placement grows away from the window's
        // bottom edge instead of past it.
        preferAbove: true
        onOpened: timeline.claimTransientInteraction("readers")
        onClosed: timeline.releaseTransientInteraction("readers")
        // Content-sized, never share-sized (2026-08-19 feedback): this is
        // a small info card, not a picker — two readers must not get a
        // 40%-of-the-window box of empty space. The card hugs its rows
        // (the list reports its real content height) and caps at half the
        // window; past the cap the list scrolls.
        width: Math.min(280, maxWidth)
        height: {
            var want = (contentItem ? contentItem.implicitHeight : 0)
                       + topPadding + bottomPadding
            var cap = Math.min(maxHeight,
                               overlayItem ? overlayItem.height * 0.5 : 400)
            return Math.max(Math.min(want, cap), 96)
        }
        // The shared floating-popover surface (EmojiPicker/GifPicker
        // precedent) — without it the popup fell through to the Basic
        // style's flat unthemed box (2026-08-19 design audit P0).
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.menuRadius + 6
        }
        // Element-parity read time (2026-08-19 request): today -> time,
        // this week -> weekday + time, older -> date + time. tsMs 0 means
        // the receipt carried no timestamp — render nothing, never a
        // fabricated time.
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
                // Element's exact header wording — the %n-source-string
                // form renders its "(s)" literally without a loaded
                // translation, so the plural is branched explicitly.
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
                // The card's content-driven height reads this: the list's
                // real content height, not the 0 a ListView reports by
                // default.
                implicitHeight: contentHeight
                clip: true
                spacing: 2
                model: receiptListPopover.readers
                // Shared themed bar. A stock ScrollBar is the Basic style's,
                // which paints from the OS palette (main.cpp sets the Basic
                // style and never installs a QPalette), so it neither follows
                // the selected Lightning theme nor changes when the theme
                // does.
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
                    // An invisible footer still occupies its height in
                    // contentHeight — collapse it, or every short list
                    // carries a ghost row in the content-sized card.
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
            // Release the tone level first: when the picker closes while the
            // tone popup is up, the owner is "tone", and only that release
            // matches.
            timeline.releaseTransientInteraction("tone")
            timeline.releaseTransientInteraction("picker")
            targetEventId = ""
        }
        // The nested skin-tone popup is modal in its own right (see
        // EmojiPicker), and it owns row interaction while it is up.
        onToneOpened: timeline.claimTransientInteraction("tone")
        // The fallback is unconditionally "picker" and both close orders
        // still converge: if the picker closes FIRST it releases "tone"
        // itself (leaving nobody), and this handler then matches nothing.
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
    // Room-scoped invite, opened from the empty-room block. A separate
    // instance from Space Home's (which is scoped to the Space it is showing
    // and lives inside a Loader that is not active while a room is open), so
    // neither can inherit the other's target room.
    InvitePeopleDialog {
        id: roomInviteDialog
        objectName: "roomInviteDialog"
        parent: Overlay.overlay
    }

    // Every floating surface that is anchored to, or snapshotted from, a
    // timeline row. ONE helper so the callers cannot drift: a room/account
    // switch closes them, and so does anything else that discards timeline
    // content — notably the jump-to-live history trim, which resets the
    // model WITHOUT changing the room, so none of the signal-driven cleanup
    // below would fire for it (review finding, 2026-08-19).
    function closeRowAnchoredSurfaces() {
        sharedReactionPicker.close()
        senderProfilePopover.close()
        receiptListPopover.close()
        // The viewer holds decoded pixels and a stale entries snapshot; it
        // must never survive content it was opened from.
        imageViewer.close()
        // The single reset point for transient row-interaction ownership.
        // Every close above releases its own claim, but a surface that was
        // never opened through those handlers (or one destroyed under the
        // pointer) would otherwise leave the timeline permanently unable to
        // show an action bar. Deliberately not a second cleanup path — this
        // helper is already the one every switch and content discard calls.
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
            // Fall back to any real, currently-instantiated row — walk back
            // from the newest loaded row. openContextMenu() itself no-ops
            // for a virtual/state-activity row or one with no real event id
            // (it never sets menuEventId), so this backward scan skips
            // those without any special-casing here.
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
    // v0.6.6: save/unsave GIF feedback — the same auto-clearing
    // banner Save As already uses (an explicit-export action of the same
    // class; see GifStarredStore's header). Honest failures only: no silent
    // drop. `message` is ALREADY a translated, ready-to-display sentence
    // (see GifStarredStore::categoryMessage) — never a raw category token.
    Connections {
        target: app.gif.starredStore
        // v0.6.7: FAILURES ONLY. A successful save or unsave says nothing —
        // the star itself fills or empties, which is feedback in the place the
        // user is already looking, and the banner is one more thing appearing
        // and disappearing at the bottom of the timeline. Honest failures are
        // unaffected: `message` is already a translated, ready-to-display
        // sentence (GifStarredStore::categoryMessage), never a raw category
        // token, and nothing is silently dropped.
        //
        // "already_starred" is reported as ok, so it is silent too. The star
        // was showing UNfilled when the user pressed (that is why the durable
        // check took the star branch at all), but MessageDelegate.qml's own
        // onStarFinished calls refreshStarredState() regardless of `ok`, so
        // the star fills on completion either way — that fill is the feedback,
        // and it is more accurate than a banner would be.
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
        objectName: "roomColumn"
        // Deliberate narrow fallback: below 660px pane width the 340px
        // panel plus the 320px timeline minimum cannot coexist, so the open
        // thread surface takes the pane (the room and its state stay alive
        // underneath and return when the panel closes or the window
        // widens). From 660px up, threads are ALWAYS a side panel.
        visible: !(root.threadSurfaceOpen && root.width < 660)
                 && !(root.searchOpen && root.width < 700)
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 320
        spacing: 0

        // Room header — design: 60px, 0 20px padding, 34px room avatar,
        // bare 34×34/radius-8 icon buttons; the open right panel's toggle
        // shows the accent-chip state.
        Rectangle {
            objectName: "roomHeaderBand"
            Layout.fillWidth: true
            implicitHeight: 60
            // Nothing in a 60px band may be drawn outside it. The band is
            // followed in this column by the timeline, which paints AFTER
            // it, so anything that escapes downward is both visible over the
            // messages and unclickable behind them. That is what a room
            // topic carrying newlines did (see the identity block below).
            clip: true
            // `sidebar`, the PANE tone — not `surface`, the CARD tone.
            //
            // Under Storm both the room header and the composer card resolved
            // to the same literal, so a card with a radius and a border was
            // exactly the colour of the wall behind it: measured 1.000:1. One
            // hex was painting the chrome AND the content, which is most of
            // what "the message box and the imbeds are very pale" meant — the
            // embeds were not low-chroma, they had no relationship to
            // anything. On `sidebar` the header runs continuous with the room
            // list beside it (one top strip, as Element has), the composer
            // card lifts off it at 1.272, and the 1px stormBorder already
            // under this row keeps the edge. Both inks the header uses are
            // already asserted against this surface.
            color: AppTheme.sidebar
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
                    // One fallback-colour policy (was keyed by display
                    // NAME here — a third colour for the same DM partner,
                    // changing on rename).
                    colorKey: root.currentRoom.identityColorKey
                              || app.currentRoomId
                    // Shape rule: people/DMs are circles; rooms and Spaces
                    // are rounded squares.
                    circle: root.currentRoom.isDirect === true
                }
                ColumnLayout {
                    objectName: "roomHeaderIdentity"
                    spacing: 2
                    Layout.fillWidth: true
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Label {
                            text: {
                                if (root.currentRoom.name)
                                    return root.currentRoom.name
                                if (app.currentRoomId !== "")
                                    return app.currentRoomId
                                // No room open: Space Home shows the Space's
                                // own name, not the literal "Home".
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
                            Layout.maximumWidth: header.width * 0.5
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
                        // ONE line, whatever the server sent. A room topic
                        // is free text and routinely carries newlines: an
                        // eight-line topic made this Label eight lines tall,
                        // the identity column with it, and the header's
                        // action icons were centred on that and landed in
                        // the message list, unclickable under the timeline.
                        // Reported 2026-09-02 with a screenshot; measured in
                        // timeline-pane-qml at 109px of header inside a 60px
                        // band. elide alone does not do this: Text breaks on
                        // an explicit newline whatever the elide mode.
                        text: (root.currentRoom.topic || "")
                                  .replace(/\s+/g, " ").trim()
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        visible: text.length > 0
                        Layout.fillWidth: true
                        elide: Label.ElideRight
                        maximumLineCount: 1
                    }
                    // Clicking the header identity opens Room Information.
                    TapHandler {
                        enabled: app.currentRoomId !== "" && app.roomInfo.supported
                        onTapped: root.toggleRoomInfo()
                    }
                }
                Item { Layout.fillWidth: true }
                RowLayout {
                    objectName: "roomHeaderActions"
                    spacing: AppTheme.spacing6
                    IconButton {
                        objectName: "startVoiceCallButton"
                        // 1:1 DMs only: a legacy m.call.invite rings every
                        // member of the room, so a group room must never
                        // get this button. Idle/Ended only — one call at a
                        // time, and the corner card owns a live one.
                        // ONE policy question, answered in AppController:
                        // MatrixRTC where available (video, screen share,
                        // groups — what Element speaks), the legacy 1:1 lane
                        // as the audio-only DM fallback. The button is
                        // absent when neither can carry a call, rather than
                        // present and dead.
                        visible: app.currentRoomId !== ""
                                 && app.canStartCall(app.currentRoomId)
                                 && !app.groupCall.active
                                 && (app.calls.state === CallController.Idle
                                     || app.calls.state
                                        === CallController.Ended)
                        // 2026-08-23: enabled, and its VISIBILITY now asks
                        // AppController whether either lane can actually
                        // carry a call — so a packaged build without the
                        // GStreamer plugins, or a homeserver with no
                        // MatrixRTC and a non-DM room, shows no button at
                        // all rather than a dead one.
                        enabled: true
                        iconName: "call"
                        Accessible.name: qsTr("Start a voice call")
                        ToolTip.text: qsTr("Start a voice call")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: app.startCall(app.currentRoomId, false)
                    }
                    // Pinned-messages shortcut: shown only when the room
                    // actually has pins, so users reach the list in one
                    // click instead of Room Information → Pinned.
                    IconButton {
                        objectName: "pinnedMessagesButton"
                        visible: app.currentRoomId !== ""
                                 && app.roomInfo.supported
                                 && app.pinned
                                 && app.pinned.supported
                                 && app.pinned.roomId === app.currentRoomId
                                 && app.pinned.total > 0
                        iconName: "push_pin"
                        active: root.infoOpen && infoPanel.section === "pinned"
                        Accessible.name: qsTr("Pinned messages")
                        ToolTip.text: qsTr("Pinned messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.togglePinnedPanel()
                    }
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
                        active: root.searchOpen
                        Accessible.name: qsTr("Search messages")
                        ToolTip.text: qsTr("Search room messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleSearchPanel()
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

        // In-call controls, directly under the room header (2026-08-23,
        // maintainer request with a reference screenshot). A call is the
        // thing the user is doing in this room, so its controls belong with
        // the room rather than floating in a corner where they compete with
        // passive prompts for the same space.
        //
        // Serves BOTH lanes: the legacy 1:1 call that carries audio today
        // and the MatrixRTC group call. Collapses to zero height when no
        // call is live in THIS room, so a room without one reserves nothing.
        CallHeaderBar {
            objectName: "timelineCallHeaderBar"
            Layout.fillWidth: true
            // The participants button opens the room's existing side panel
            // rather than a second list of its own: who is in the call is
            // already on the stage as tiles, and what the button is really
            // for is reaching the people in the room.
            onParticipantsRequested: root.infoOpen = !root.infoOpen
        }

        // The call surface is a PANEL AT THE TOP of the conversation column,
        // with the message list still visible and scrolling independently
        // beneath it and a draggable divider between them. Discord's DM
        // arrangement, and what the maintainer asked for: "calls get put at
        // the top of the screen".
        //
        // It used to REPLACE the timeline — the stage filled the column and
        // the timeline and composer were hidden behind
        // `visible: !callStageOwnsColumn`. That was itself a fix for a worse
        // bug (both were `Layout.fillHeight` in one ColumnLayout, so the
        // column was SPLIT and the stage got a ~45 px strip). The ownership
        // condition survives; what changed is that the stage now takes a
        // BOUNDED, explicitly assigned height instead of `fillHeight`, so
        // there is nothing left for the two to fight over and the timeline
        // can stay visible.
        //
        // WHY THE READER'S MESSAGE DOES NOT MOVE WHEN A CALL STARTS. The
        // timeline is a 180-degree-rotated Flickable over one Column
        // (§16): view row 0 is the NEWEST message at content y 0, and
        // contentY grows going INTO history — so the content is pinned to
        // the BOTTOM edge of the viewport. This change alters the timeline's
        // HEIGHT only. Its WIDTH is untouched, so no row relayouts and
        // `contentHeight` does not change; nothing here writes `contentY`;
        // and with the content bottom-anchored, taking space off the TOP
        // reveals or hides rows at the top while the row the reader is
        // looking at stays exactly where it is. The one case that does move
        // is the panel SHRINKING far enough that `contentY` exceeds the new
        // `contentHeight - height` — the Flickable clamps, because there is
        // no longer that much history above the reader. That is the same
        // thing a window resize does and it is not avoidable.
        //
        // Second-order effect, stated rather than hidden: `distanceFromTop()`
        // is `wheelMaxY() - contentY` and `wheelMaxY()` folds in the
        // viewport height, so GROWING the timeline (call ends, panel
        // collapses) can move the reader INTO the near-top pagination band
        // and dispatch a backfill with no visible movement. A backfill is not
        // a reader displacement, and it is the same behaviour as making the
        // window taller.
        Loader {
            id: callStageHost
            objectName: "timelineCallStageHost"
            Layout.fillWidth: true
            // BOUNDED, never fillHeight: the timeline below keeps that.
            Layout.preferredHeight: active ? root.callPanelHeight : 0
            active: root.callStageOwnsColumn
            visible: active
            sourceComponent: CallStage {
                collapsed: root.callPanelCollapsed
                // The dock's participants button reaches the room's existing
                // side panel, exactly as the header bar's does — one list,
                // not a second one belonging to the stage.
                onParticipantsRequested: root.infoOpen = !root.infoOpen
                onCollapseToggled: root.callPanelCollapsed = !root.callPanelCollapsed
            }
        }

        // The divider, and the drag that resizes the call panel.
        //
        // Hand-rolled rather than a SplitView because putting the timeline
        // into a SplitView would restructure a 7000-line ColumnLayout whose
        // scrolling machinery is the most-reverted code in this repository.
        // The SplitView LESSON still applies verbatim and is why the store
        // happens where it does: a resize RELEASE moves nothing, so it emits
        // no heightChanged, and a handler hung off the height would never see
        // the end of the gesture. The value is therefore written on the
        // FALLING EDGE of the drag's own `active`.
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
                        // THE FALLING EDGE. Nothing moves on release, so this
                        // is the only moment that can commit the gesture.
                        root.callPanelUserHeight =
                                root.clampCallPanelHeight(root.callPanelHeight);
                    }
                }
                // `activeTranslation`, not `translation`: it is the movement
                // since THIS drag began and resets on each new one, which is
                // exactly what a base-plus-delta resize wants. (`translation`
                // is the deprecated 6.0 spelling.)
                onActiveTranslationChanged: {
                    if (!dividerDrag.active)
                        return;
                    root.callPanelUserHeight = root.clampCallPanelHeight(
                                root.callPanelDragBase
                                + dividerDrag.activeTranslation.y);
                }
            }
        }

        // 2026-08-23 MatrixRTC — "N people in call".
        //
        // An ordinary Layout child for the same reason roomUpgradeBanner is
        // one: a live call is PERSISTENT state, so it must reflow the
        // timeline rather than occlude messages. Placed ABOVE the upgrade
        // banner because a call in progress is the more urgent of the two.
        //
        // Purely observational — it reports the room's MatrixRTC membership
        // (typically started by an Element client) and produces no timeline
        // rows of its own. It collapses to zero height when there is no
        // call, so a room without one reserves no space.
        RoomCallBanner {
            objectName: "timelineRoomCallBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: visible ? AppTheme.spacing8 : 0
            roomId: app.currentRoomId
        }

        // v0.7.x room upgrades — the banner half of banner-and-link.
        //
        // Matrix leaves an upgraded room in place and creates a
        // replacement. Lightning does NOT follow that automatically: the
        // old room stays open and readable and the successor is offered,
        // because a room transition discards navigation context and draft
        // state, and the tombstone naming the successor is state anyone
        // with the power level can send.
        //
        // An ordinary Layout child, exactly like findBar below and for the
        // same reason: this is PERSISTENT, so it must reflow the timeline
        // rather than occlude it (the zero-height overflow trick further
        // down is documented as being for transient feedback only).
        //
        // The wording is Lightning's own. The tombstone's `body` is free
        // text chosen by whoever sent the state event, on a control the
        // user is being invited to click — it never crosses the FFI at all.
        Rectangle {
            id: roomUpgradeBanner
            objectName: "roomUpgradeBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            // A room can be BOTH an opened successor and the predecessor of
            // another room, so the two rows are independent rather than
            // exclusive.
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

                // The failure is shown HERE, in the old room, because the
                // user must be able to see why Continue did not work
                // without having been moved anywhere.
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

                    // Both rows can be visible at once (a room may be a
                    // successor AND a predecessor), so this one carries its
                    // own leading glyph rather than hanging unaligned beside
                    // the upgraded row's.
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
                        // No join step: the predecessor is a room the user
                        // was already in.
                        onClicked: app.roomUpgrade.goToPredecessor()
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open the previous room")
                    }
                }
            }
        }

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
                // Loaded-messages find vs server history search. The
                // history segment exists only where it can be honest.
                SegmentedControl {
                    objectName: "findModeToggle"
                    visible: root.findHistoryAvailable
                    dense: true
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
                            // Review M1: the controller is SHARED with the
                            // global dialog, which rescopes it to "". Every
                            // find-bar dispatch re-asserts this room, so a
                            // dialog round-trip can never make the bar show
                            // other rooms' results under this room's label.
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

            // v0.7.x: history-mode results (server /search, this room).
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
                            // The shared navigation path: paginate until the
                            // event is loaded, then centre + highlight. Deep
                            // history past its bounded window reports its
                            // honest "unavailable" message.
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
            // Stays visible THROUGH a call. It used to stand down entirely
            // (`visible: !root.callStageOwnsColumn`) because the stage was
            // `Layout.fillHeight` and the two split the column between them,
            // squashing the stage into an unusable strip. The stage now takes
            // an explicitly assigned, bounded height, so there is nothing left
            // to fight over and the messages can keep scrolling under the
            // call — which is the whole point of the change.
            visible: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ── Solid timeline: no height virtualization ─────────────────
            // Every loaded row is a real, instantiated item inside one Column,
            // so contentHeight and every row position are MEASURED. There is
            // no per-row estimate, no rowHeightProvider, no content-height
            // reconciliation and no delegate recycling anywhere in the room
            // timeline.
            //
            // This replaced a reversed TableView, and the reason is structural
            // rather than a matter of tuning. Two properties of that design
            // could not be fixed from the outside:
            //
            //  1. QQuickTableViewPrivate::rowsInsertedCallback schedules
            //     RebuildOption::ViewportOnly for an insert ANYWHERE in the
            //     model. That releases every visible delegate to the reuse
            //     pool and re-binds it, so each pagination page re-ran
            //     onReused, the height-seed settle, every nested Loader and
            //     every media identity check for every row on screen. That is
            //     a full viewport rebuild per page, and it is the stall the
            //     reader feels as history loads. Element never does this;
            //     prepending does not touch what is already rendered.
            //  2. contentHeight had to be supplied from metadata ESTIMATES
            //     while TableView laid the loaded rows out at their real
            //     heights. Two coordinate systems that disagree by
            //     construction, with origin/endExtent silently absorbing the
            //     difference. Every position the app computed lived in the
            //     estimated frame; everything the reader saw lived in the real
            //     one. Repeated attempts to reconcile them all failed.
            //
            // With a Column both problems cease to exist rather than being
            // balanced. Older history is a proxy APPEND, so it extends the
            // column at the tail: no item already positioned can move, and
            // contentY needs no correction at all for pagination. Rows keep
            // their identity for as long as they are loaded, so nothing is
            // ever rebound underneath the reader.
            //
            // The cost is that the whole loaded window is instantiated. That
            // is Element's trade too. Rows arrive ~20 at a time as the reader
            // pages, never in one burst, and a hidden row (a filtered activity
            // line) is skipped by the positioner and occupies exactly zero
            // height with no special case.
            Flickable {
                id: timeline
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
                // Rotate the viewport and counter-rotate each row: proxy row 0
                // (newest) sits physically at the bottom, and older history
                // extends the far/top edge.
                rotation: 180
                flickableDirection: Flickable.VerticalFlick
                // The presentation gate covers the view (it keeps laying out
                // underneath so viewport-fill pagination and positioning run
                // against real geometry); the loading surface sits on top.
                opacity: presentationReady ? 1 : 0
                // The source stays chronological for every backend and
                // non-visual consumer. The proxy exposes newest-to-oldest.
                readonly property int count: rowRepeater.count
                contentWidth: width
                contentHeight: rowColumn.height

                // ── Which rows may activate their heavy content ──────────
                // Every loaded row is instantiated now, so there can be many
                // hundreds of live delegates. Each one asking "am I on screen?"
                // in terms of contentY re-evaluates that binding on EVERY row
                // on EVERY pixel scrolled — hundreds of float comparisons per
                // frame that all answer the same question. That was a large
                // part of the timeline feeling heavy overall.
                //
                // Compute the range ONCE per turn instead and let each row do a
                // pair of integer comparisons against it. The range only
                // changes when a row boundary is crossed, so the per-row
                // bindings stay quiet through most of a gesture.
                //
                // One viewport of slack on each side means content activates
                // before it is reached rather than popping in at the edge.
                property int visibleFirstRow: 0
                property int visibleLastRow: -1
                // A child Timer rather than Qt.callLater: the timer dies with
                // the pane, whereas a queued callLater still fires after the
                // object has been torn down (room switch, logout) and throws
                // because its QML methods are already gone. restart()
                // coalesces repeat requests for free.
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
                    // Inset by the side margins directly. A Flickable's
                    // left/right margins only widen the flickable RANGE; they
                    // do not lay content out, and this view never scrolls
                    // horizontally. (The 180° rotation swaps left and right,
                    // which is harmless while the two are equal.)
                    x: timeline.leftMargin
                    // Rows can be built before the pane has its final width. A
                    // non-positive width makes a long wrapped body measure as
                    // one character wide, producing an enormous transient
                    // height. Fall back to a normal message column until the
                    // real viewport width is positive.
                    width: {
                        var available = timeline.width - timeline.leftMargin
                                      - timeline.rightMargin
                        return available > 0 ? available : 640
                    }
                    // Delegates own sender-group spacing: group leaders
                    // receive a compact break while continuations stay
                    // visually glued together. A global gap made every
                    // continuation look like an unrelated row.
                    spacing: 0

                    // Qt's own "a positioning pass just finished" signal.
                    // How many rows that pass covered is what a
                    // geometry-reading jump path needs before it can trust a
                    // delegate's y: children are placed during polish, so a
                    // row created or shifted in this turn still reports the
                    // previous shape's coordinates. See
                    // navigationGeometryReady(). Nothing binds to the
                    // property, so writing it from here cannot loop.
                    onPositioningComplete: {
                        timeline.layoutRowsAtLastPass = timeline.count
                        // The Column has just re-positioned its CURRENT
                        // children, so contentHeight (bound to this Column's
                        // height) now describes THIS snapshot rather than the
                        // outgoing room's lingering rows. That is precisely
                        // what presentationGeometryStale means, so this — not
                        // a timer — is where it is allowed to clear.
                        //
                        // It replaces a 0 ms settle timer that waited on
                        // presentationResetPending instead. Waiting for that
                        // flag is NOT the same as waiting for the relayout:
                        // the timer could fire while contentHeight was still
                        // the previous snapshot's, fillsViewport then read
                        // true on a one-item partial snapshot, and the gate
                        // opened on it — re-introducing the exact defect the
                        // 2026-08-18 round fixed (measured then as
                        // count=1 ch=3601 h=404). timeline-hydration-qml
                        // caught it.
                        //
                        // Unlike onContentHeightChanged this also fires when
                        // the new height happens to EQUAL the old one, which
                        // is the case the settle timer was reaching for: an
                        // incoming room whose Column height matches the
                        // outgoing room's otherwise kept the flag armed for
                        // that whole room generation.
                        if (timeline.presentationGeometryStale) {
                            timeline.presentationGeometryStale = false
                            timeline.recomputePresentationReady()
                        }
                    }

                    Repeater {
                        id: rowRepeater
                        model: app.timelineView
                        // Rows are built SYNCHRONOUSLY and are direct children
                        // of the Column. Wrapping them in an asynchronous
                        // Loader was tried and was much worse: an incubating
                        // row has zero height until it finishes, so a page
                        // materialised out of order and visibly squeezed the
                        // column, and the extra item plus incubation overhead
                        // per row made everything slower rather than smoother.
                        delegate: MessageDelegate {
                            width: rowColumn.width
                            rotation: 180
                            // No attached view exists inside a Repeater, so
                            // the row's view reference is injected. This also
                            // deliberately withholds the height-seed/recycling
                            // API (cachedDelegateHeight, rememberDelegateHeight)
                            // that only the reused ListView path needs, so the
                            // delegate falls through to its own natural height
                            // — which here is simply the truth.
                            timelineView: timeline
                            // An INDEX-RANGE test, never a geometry test. See
                            // visibleFirstRow below: with every loaded row
                            // instantiated, a per-row binding on contentY costs
                            // one float comparison per row per pixel scrolled.
                            rowOnScreen: index >= timeline.visibleFirstRow
                                         && index <= timeline.visibleLastRow
                        }
                    }
                }

                // NOTE: there is deliberately no height model here any more.
                // The Column measures itself; contentHeight above is that
                // measurement. Row insertions need no bookkeeping because an
                // append cannot move an item that is already positioned.
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
                // v0.6.7, corrected: the avatar gutter is `rightMargin`, NOT
                // `leftMargin`. Two effects compound here and the first
                // attempt at this missed both:
                //
                //   1. `rotation: 180` above swaps left and right, so the
                //      LOGICAL right margin is the VISUAL left inset;
                //   2. at its left bound a Flickable parks contentX at
                //      -leftMargin, which offsets the content item by
                //      +leftMargin on top of rowColumn's own
                //      `x: timeline.leftMargin`.
                //
                // Net: visual left inset == rightMargin - leftMargin. With
                // both at 12 that was 0, which is why the avatars read as
                // glued to the SplitView handle. Raising leftMargin to 20
                // made it -8 and pushed the avatar column out over the
                // divider — the reported regression. The gutter has to come
                // from the far side.
                //
                // Visual right inset is 2 * leftMargin (24), unchanged.
                readonly property real avatarGutter: 20
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM + avatarGutter

                // Auto-scroll to end on new events when already near the bottom.
                // Read defensively: several timeline suites construct this
                // pane without an `app` context property, and an undefined
                // read here would take the whole wheel handler down rather
                // than merely un-animating it.
                readonly property bool smoothScrollingEnabled: {
                    // Explicitly coerced, never handed through raw. A stub `app.settings`
                    // that does not carry this property yields UNDEFINED, and assigning
                    // undefined to a bool is a QML warning — which the GIF picker suites
                    // correctly treat as a failure. Defaulting to TRUE also keeps the
                    // shipped feel for any surface whose settings object is incomplete.
                    if (typeof app === "undefined" || !app || !app.settings)
                        return true
                    var v = app.settings.smoothScrolling
                    return v === undefined ? true : !!v
                }

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
                    // While the row window hides the live edge, the physical
                    // bottom of the view is NOT the newest message — reporting
                    // "at bottom" there would pin follow-latest to the wrong
                    // place and hide the jump pill (2026-08-19).
                    if (rowWindowSkip > 0)
                        return false
                    return atYBeginning
                           || contentY <= wheelMinY() + bottomFollowSlack
                }
                readonly property int rowWindowSkip:
                    app.timelineView && app.timelineView.windowSkip !== undefined
                    ? app.timelineView.windowSkip : 0

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
                // Set while a room reset is in flight. Rows are built into the
                // Column synchronously now, so contentHeight moves DURING the
                // reset and onContentHeightChanged would recompute the gate
                // right then — reading the outgoing room's settled state and
                // opening on a one-item partial snapshot, which is the exact
                // defect the gate exists to prevent. Under the previous
                // virtualized view contentHeight was application-owned and only
                // updated a turn later, so the deferred recompute was enough on
                // its own. It no longer is.
                property bool presentationResetPending: false
                // True from a model reset until the first contentHeight
                // change afterwards — i.e. until the Column has produced a
                // geometry that reflects the NEW snapshot rather than the
                // outgoing room's rows. While stale, fillsViewport is not
                // trustworthy; the settled/guard paths still open the gate.
                property bool presentationGeometryStale: false
                // Whether the view held any rows when the last model reset
                // was announced — see onModelAboutToBeReset.
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
                    // real height AND the Column has re-laid-out since the
                    // last model reset: right after a reset it still reads
                    // the OUTGOING content's height (old delegates linger
                    // until their deferred destruction), so trusting it
                    // opened the gate on a one-item partial snapshot — the
                    // exact defect this gate exists to prevent. With height
                    // 0 the >= comparison is degenerately true as well.
                    var fillsViewport = count > 0 && height > 0
                                        && !presentationGeometryStale
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
                // geometry pass. maintainViewAnchor() re-bases this
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
                // Row index, content height, and ListView origin at the last
                // measurement. When
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
                // Diagnostic baseline for the next live-scroll experiment.
                // ListView moves originY when rows are prepended, but it also
                // moves it while revising the average-height estimate for
                // delegates it has not built. Recording it beside the proven
                // row-index increase lets a physical trace distinguish those
                // two causes without changing any scroll correction yet.
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
                        // Diagnostics only: distinguishes "the mechanism ran
                        // and had nothing to correct" (every other counter
                        // legitimately 0) from "the mechanism never engaged
                        // at all" — see the M2 comment on diagNoAnchorReturns.
                        // The two early-return causes are counted SEPARATELY,
                        // not merged — "no anchor captured yet"
                        // (viewAnchorId empty: a real gap in anchor
                        // coverage, e.g. a gesture that started before
                        // captureViewAnchor() ever ran) and "returned to the
                        // bottom before this coalesced call fired"
                        // (stickToBottom). The second is NOT the routine
                        // bottom-pinned case: this function is never
                        // scheduled while stickToBottom is already true, so
                        // it can only count a correction scheduled while
                        // scrolled up and then dropped on return-to-bottom.
                        // Order matters and is deliberate: an empty id wins
                        // when both hold.
                        if (scrollTrace) {
                            if (viewAnchorId === "")
                                diagNoAnchorReturns += 1
                            else
                                diagStickToBottomReturns += 1
                        }
                        return
                    }
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
                    var row = viewRowForStableId(viewAnchorId)
                    var it = row >= 0 ? itemAtViewRow(row) : null
                    var anchorY = anchorPositionForItem(it)
                    // Diagnostics only. A stable row index increasing is the
                    // proof that rows were inserted before the anchor. On that
                    // exact firing, pair the signed origin shift candidate
                    // (`old originY - new originY`) with the whole-content
                    // delta currently used by the displaced branch. This is
                    // the missing live measurement: originY may be quieter,
                    // equally noisy, or worse, and production behavior must
                    // not guess which before the physical trace answers it.
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
                        // DISPLACED by an insertion above the reader. The row
                        // index rising is the proof: rows were inserted
                        // before it, which at the top edge pushes it clear of
                        // the delegate cache so Qt destroys it. This estimate-
                        // based fallback is IDLE-ONLY: active input falls
                        // through to the measurement-only re-capture below,
                        // because a real trace proved this quantity can cancel
                        // essentially an entire upward wheel gesture.
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
                        // Diagnostics only (see the diagDisplaced* block
                        // above) — discriminates the reviewer's H1: whether
                        // this branch fires at all during a real jitter
                        // gesture, and whether |grew| stays proportionate to
                        // insertedRows (real growth) or spikes far beyond
                        // what that many rows could plausibly account for
                        // (the estimate-noise hypothesis). Recorded on EVERY
                        // entry, including the skipped-negative case, so a
                        // real trace can show the swing this branch actually
                        // saw even when nothing was applied. Gated on
                        // scrollTrace, not diagActive — see the M1 comment on
                        // the diagDisplaced* declarations for why this must
                        // survive past the gesture that triggered it.
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
                        // Genuinely unresolvable — the stable id no longer
                        // exists (redaction, local-echo id change), or a
                        // native drag owns the view. Re-measuring a fresh
                        // anchor writes no position (pure measurement), so it
                        // is safe even mid-gesture; the next call's delta is
                        // then measured from this new baseline rather than a
                        // stale, unresolvable one.
                        // Diagnostics only (see the L1 comment on the
                        // diagUnresolvedIdFallbacks/diagEvictedNoInsertFallbacks
                        // declarations): split by whether the stable id
                        // still resolves to a real row at all. row < 0 means
                        // it does not (redaction, local-echo id swap, or a
                        // genuinely stale/unknown id) — a true "give up and
                        // re-derive". row >= 0 means the id is fine and the
                        // delegate was simply evicted from the cache with no
                        // row-index proof anything was inserted above it
                        // (including the moving===true case, where the
                        // displaced branch's arithmetic path is blocked even
                        // if row > viewAnchorRow, and the viewAnchorRow < 0
                        // case where no row was ever recorded) — expected to
                        // dominate in a media-heavy room on ordinary
                        // scrolling alone. Counters are not reset on a room
                        // switch, so the first line after one can carry the
                        // previous room's outcomes ("since the previous
                        // line" applies across rooms too).
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
                        // deferredDelta is how far the anchor row's own y has
                        // moved since it was last measured. Screen position is
                        // (item y - contentY), so moving contentY by the same
                        // amount holds the reader's message exactly where it
                        // was while the rows around it resize.
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
                        // NO WRITE. This has now been tried twice and rejected
                        // by physical testing twice, the second time with the
                        // height cache alive and with translateActiveMotion()
                        // carrying the wheel target along — so neither "the
                        // quantity was estimate noise" nor "the engine drove
                        // the correction back out" explains it. Applying it
                        // pulled the reader both up AND down during loading,
                        // and down during ordinary scrolling with nothing
                        // loading at all.
                        //
                        // The lesson is about WHAT the delta measures, not how
                        // it is applied: anchorY moves for reasons that are not
                        // displacement of the reader. TableView re-anchors the
                        // loaded table on its own rebuilds, so the anchor row's
                        // y can change while the reader's view of it did not.
                        // Feeding that into contentY injects motion the reader
                        // never asked for. Do not re-enable this without a
                        // measurement that separates "the row moved under the
                        // reader" from "the table was re-anchored beneath both
                        // of them" — the raw delta cannot tell them apart.
                        //
                        // While input owns the viewport, accept Qt's live
                        // layout position and re-base the measurement. The next
                        // geometry change is measured from here, and settle
                        // captures the final reading position once more.
                        captureViewAnchor()
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
                    // after input has gone quiet, just via the absolute form,
                    // which is only safe because nothing is competing for
                    // contentY.
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

                // v0.6.0: MessageDelegate view contract — the room timeline
                // resolves stable-id actions against app.timeline and never
                // suppresses a row as a pinned thread root.
                property var timelineModel: app.timeline
                // Hand over the proxy's paced backlog immediately. Any path
                // that must address a specific event by row calls this first;
                // see ReverseListProxyModel::releaseAll().
                // Every jump/search/permalink path calls this before it
                // addresses a row by id. It must restore the LIVE EDGE, not
                // merely the paced backlog: `releaseAll()` lifts the pacing
                // cap but leaves the row window's skip in place, so with a
                // window active a jump to any RECENT message resolved to "no
                // such row" and silently did nothing — the same silent
                // failure the pacing backlog already taught this codebase
                // once. `clearWindow()` resets the skip AND releases the
                // backlog (self-review find, 2026-08-19).
                function releasePendingRows() {
                    if (!app.timelineView)
                        return
                    if (app.timelineView.clearWindow)
                        app.timelineView.clearWindow()
                    else if (app.timelineView.releaseAll)
                        app.timelineView.releaseAll()
                }
                // View row <-> source row. The reversal is anchored on the
                // SOURCE total, never on `count`: the proxy paces newly
                // paginated history out over a few frames, so it can briefly
                // expose fewer rows than the model holds, and those two
                // numbers are then not the same. `count` still bounds which
                // view rows exist. Deriving the mapping from `count` instead
                // would renumber every visible row for as long as a page was
                // draining.
                // 2026-08-19: the row WINDOW shifts the newest edge, so the
                // reversal is anchored on (source total - 1 - windowSkip),
                // not on the source total alone. Getting this wrong is
                // silent: a jump or an anchor restore simply resolves to "no
                // such row" and does nothing — which is exactly how the
                // window's own anchor correction failed its first test run.
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
                // Every loaded row is instantiated, so this is a direct lookup
                // rather than "the delegate IF the virtualizer happens to have
                // built it". Nothing downstream has to handle a null for a row
                // that merely scrolled out of a cache buffer any more.
                function itemAtViewRow(row) {
                    return row < 0 || row >= count
                            ? null : rowRepeater.itemAt(row)
                }
                // Which row occupies a given content-space y. Rows are laid out
                // in ascending y by the Column, so this is a binary search over
                // real geometry — no average-size guess anywhere in it. Hidden
                // rows have zero height and are simply never the answer.
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
                    // of the visible content range.
                    return viewRowAtContentY(
                        contentY + Math.max(0, height - topMargin - 1))
                }
                // The view is rotated: the logical bottom edge of a row is its
                // physical top edge. Anchor that edge so row-height changes
                // retain the message under the reader.
                function anchorPositionForItem(item) {
                    return item ? item.y + item.height : 0
                }
                // Row 0 is the newest message and sits at content y 0, which
                // the rotation puts at the physical bottom. Following the
                // latest is therefore just the low bound — no alignment enum,
                // no second settling pass to correct an estimate.
                function positionViewAtLatest() {
                    contentY = wheelMinY()
                }
                // How far INTO the viewport a navigation target lands. A
                // reply jump that pins the quoted message flush against an
                // edge shows it with no context on one side; a third of a
                // viewport in is the Discord placement and reads as "here it
                // is", not "it is off the edge". Built on the same measured
                // geometry positionViewAtViewRow() uses plus one bounded
                // offset and the same clamp — no second anchor mechanism, and
                // maintainViewAnchor() is not involved.
                readonly property real navigationInsetFraction: 0.33
                function positionViewAtNavigationTarget(row) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return false
                    // Rotated view: a row's logical bottom edge is its
                    // physical top. anchorPositionForItem(item) - height +
                    // topMargin puts that edge at the viewport's physical
                    // top; adding the inset moves the row DOWN the screen by
                    // exactly that many pixels.
                    var target = anchorPositionForItem(item) - height
                                 + topMargin + height * navigationInsetFraction
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    contentY = target < lo ? lo : (target > hi ? hi : target)
                    return true
                }
                // Bounded navigation diagnostics — counts only, never an
                // event id. diagNavigationUnresolved is the number that used
                // to be invisible: BOTH failure paths here (C5b B1/B2) were
                // silent returns, so a jump that did nothing looked exactly
                // like one that worked, which is why reply navigation stayed
                // broken for so long with nobody able to point at a line.
                // Rows that still measured ZERO after a layout flush when
                // the row window corrected contentY. Non-zero means a window
                // move threw the reader by that much — it names this
                // mechanism instead of the anchor machinery, which three
                // reverted fixes blamed wrongly.
                property int diagWindowUnmeasuredRows: 0
                property int diagNavigationLandings: 0
                property int diagNavigationUnresolved: 0
                // Jumps the reader overrode by scrolling before they landed.
                // A healthy session shows a few; a large count next to a
                // small diagNavigationLandings means targets routinely take
                // longer to build than a reader is willing to wait.
                property int diagNavigationAbandoned: 0
                // ── The geometry half of onTargetLocated ─────────────────
                //
                // releasePendingRows() lifts the pacing backlog AND the row
                // window, so it can insert hundreds of rows in one turn. The
                // original defect was reading geometry in that same turn.
                // Both of its faces are real and only the first is obvious:
                //
                //   * the delegate may not exist yet — a Repeater incubates
                //     AsynchronousIfNested, which is synchronous only while
                //     the Repeater itself was not built inside an
                //     asynchronous incubator (MainScreen instantiates this
                //     pane directly today, so it is) — and
                //     positionViewAtViewRow() returns silently on a null
                //     item: a jump that does nothing and says nothing;
                //   * and, in every case that actually reaches this code
                //     today, the delegate DOES exist and is completely
                //     UNMEASURED. A row's height comes from its ColumnLayout,
                //     and a QQuickLayout only applies its implicit size from
                //     updatePolish(); the Column likewise positions its
                //     children during polish. So a row created in this turn
                //     reads y == 0, height == 0, anchorPositionForItem() == 0
                //     — and the landing clamps contentY to the newest end.
                //     That is worse than the silent no-op: it is a confident
                //     jump to the wrong end of the room.
                //
                // Existence is therefore NOT the readiness condition.
                // navigationGeometryReady() is, and it asks for both halves:
                // the row itself is measured, and the LAST completed Column
                // layout pass covered the row set we are about to measure
                // against (rows inserted at the newest end move every row
                // after them, so a target that was already exposed can have a
                // perfectly good height and a stale y).
                //
                // Then write contentY exactly ONCE. This is deliberately NOT
                // a correction retry loop: nothing is written until the
                // geometry is real, there is a single write, and it never
                // re-runs against the anchor machinery afterwards. A newer
                // jump REPLACES a pending one rather than queueing behind it.
                property int navigationPendingRow: -1
                // The target's STABLE ID. Authoritative while the landing
                // waits; navigationPendingRow is only the fallback for a row
                // whose id the model cannot answer. See
                // beginNavigationLanding().
                property string navigationPendingId: ""
                property real navigationPendingOffset: 0
                property bool navigationPendingHighlight: false
                property int navigationPendingAttempts: 0
                // Attempts since the landing was armed, NEVER reset by the
                // convergence re-arm below. The convergence budget exists so
                // a slow machine still lands; this is the ceiling that stops
                // it waiting forever when the view never stops changing.
                property int navigationTotalAttempts: 0
                // "<rows>/<laidOutRows>" at the previous landing attempt.
                // A change means the view is still converging, which re-arms
                // the budget; see tryLandNavigationTarget().
                property string navigationLastShape: ""
                // The row count the Column's most recent completed
                // positioning pass ran over. Equal to `count` means the
                // laid-out row set IS the current one; smaller (or larger)
                // means rows were added or removed since and every y this
                // view could read is from the previous shape. Written from
                // the Column's own positioningComplete signal — Qt's own
                // statement that a pass finished — never inferred from a
                // timer or from a row happening to sit at y == 0.
                property int layoutRowsAtLastPass: 0
                function navigationGeometryReady(item) {
                    if (!item)
                        return false
                    // A row that is deliberately not shown (hidden routine
                    // activity, a suppressed thread root, an orphan date
                    // divider) is measured AT zero and never grows, so
                    // height > 0 would wait for something that cannot
                    // happen. `visible` is a plain binding and is correct
                    // from the delegate's first turn.
                    if (item.height <= 0 && item.visible)
                        return false
                    return layoutRowsAtLastPass === count
                }
                // ~200 ms of frames. Counted in TICKS, not wall clock: a
                // machine slow enough to spend half a second building the
                // rows this release just exposed spends it inside one tick
                // rather than burning the budget. Long enough for incubation
                // and the Column's relayout, short enough that an impossible
                // target reports rather than hangs.
                readonly property int maxNavigationLandingAttempts: 12
                // ABSOLUTE ceiling, ~2 s of ticks. The convergence re-arm
                // above resets the 12-tick budget whenever the row set or the
                // laid-out row set changed since the last attempt — and while
                // the reader scrolls, BOTH change constantly (a pagination
                // batch alters `count`, the row window alters it again on
                // every settle, and each Column pass alters
                // layoutRowsAtLastPass). Without this ceiling the budget is
                // re-armed forever, the landing never expires, and it fires
                // whenever the target finally becomes measurable — which is
                // typically seconds later, mid-gesture, as a teleport back to
                // a jump the reader had already given up on.
                readonly property int maxNavigationLandingTicks: 120
                Timer {
                    id: navigationLandingTimer
                    interval: 16
                    repeat: false
                    onTriggered: timeline.tryLandNavigationTarget()
                }
                // The reader taking hold of the view abandons a jump that has
                // not landed yet. A pending landing writes contentY and
                // cancels wheel motion when it finally resolves, so leaving
                // one armed across a deliberate gesture means the view can be
                // yanked out from under the reader at an arbitrary later
                // moment. Called ONLY from genuine pointer input (wheel,
                // drag, flick, autoscroll) — never from a programmatic write,
                // which would cancel the very landing it is performing.
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
                // True once the reader has moved this room's view themselves.
                // Cleared on model reset, because the next room's view is not
                // one they have taken a position in yet.
                property bool readerControlledSinceReset: false
                // The single "the reader is driving" entry point for every
                // genuine gesture.
                //
                // Retiring the QML landing alone is not enough. A scroll
                // anchor RESTORE runs on the controller and can spend up to
                // kMaxNavigationBatches real backward paginations — five to
                // fifteen seconds — before it emits targetLocated and arms a
                // landing. That landing does not exist yet when the reader
                // starts scrolling, so there is nothing for
                // abandonNavigationLanding() to cancel; it appears later and
                // teleports them to the position the room opened at. So this
                // reaches the controller as well, and the flag catches the
                // remaining race where the restore is armed between the
                // gesture and the cancel.
                function noteReaderTookControl() {
                    readerControlledSinceReset = true
                    if (app.pagination)
                        app.pagination.cancelNavigation()
                    abandonNavigationLanding()
                }
                function beginNavigationLanding(row, pixelOffset, highlight) {
                    // Hold the target by STABLE ID, never by row number. This
                    // landing can wait up to 12 frames, and a backward
                    // pagination batch landing in that window renumbers every
                    // source row — so a retried jump held as an integer would
                    // resolve to a DIFFERENT message than the one the reader
                    // clicked, silently and confidently. The id is resolved
                    // back to a row on each attempt through the same
                    // rowForStableId() the anchor machinery already uses.
                    navigationPendingId = app.timeline
                                          ? app.timeline.stableIdAt(row) : ""
                    // Keep the row only as the fallback for a model that
                    // cannot answer a stable id for it (it answers "" then);
                    // in that case there is nothing better than the index,
                    // and a renumber is still less likely than never landing.
                    navigationPendingRow = row
                    navigationPendingOffset = pixelOffset
                    navigationPendingHighlight = highlight
                    navigationPendingAttempts = 0
                    navigationTotalAttempts = 0
                    navigationLastShape = ""
                    // Try immediately: an already-exposed target lands on
                    // this turn exactly as it always did.
                    tryLandNavigationTarget()
                }
                function tryLandNavigationTarget() {
                    if (navigationPendingRow < 0)
                        return false
                    // Re-derive the row every attempt (see the note above).
                    const row = navigationPendingId !== ""
                                && app.timeline
                                ? app.timeline.rowForStableId(
                                      navigationPendingId)
                                : navigationPendingRow
                    const viewRow = row >= 0 ? viewRowForSourceRow(row) : -1
                    const item = viewRow >= 0 ? itemAtViewRow(viewRow) : null
                    if (!navigationGeometryReady(item)) {
                        // Bound on CONVERGENCE, not on wall clock. A fixed
                        // tick budget is ~200 ms of real time, and on a
                        // loaded or slow machine the Column can still be
                        // polishing when it runs out — the reply jump then
                        // reports unresolved and the click silently does
                        // nothing, which is the very defect this landing
                        // exists to fix. (Measured: this suite is 83/83 six
                        // times idle and fails here under 16 competing CPU
                        // hogs.) The timer keeps firing on schedule while
                        // nothing progresses, so counting ticks measures the
                        // machine's load rather than the view's readiness.
                        //
                        // Progress = the row set or the laid-out row set
                        // changed since the last attempt. While either moves
                        // the view is still converging and we keep waiting;
                        // the budget is spent only on attempts that observed
                        // NO change, so a genuinely impossible target still
                        // gives up promptly instead of spinning forever.
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
                        // Honest failure instead of a silent return. Counts
                        // and the view's own shape only — never an event id.
                        // The geometry fields separate "the row was never
                        // built" from "it was built and never measured",
                        // which are different bugs with the same symptom.
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
                                     // Separates "this target can never be
                                     // measured" (ticks well under the
                                     // ceiling) from "the view never stopped
                                     // changing" (ticks AT the ceiling).
                                     + " ticks=" + navigationTotalAttempts)
                        return false
                    }
                    // The row set is laid out and this row is measured, but a
                    // height that settled AFTER that pass (an image, a link
                    // preview) leaves the Column with a polish still pending
                    // and every y below it short by that delta. forceLayout()
                    // is Qt's own synchronous flush of exactly that — one
                    // call, no waiting, no second correction afterwards — so
                    // the single write below reads final geometry.
                    //
                    // This is the POSITIONER's forceLayout (it re-runs
                    // prePositioning over children that already exist), NOT
                    // the TableView one this file warns about above: it
                    // materialises nothing, requests no media, and runs once
                    // per navigation jump rather than once per page.
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
                    // Place the row's physical top edge at the viewport's
                    // physical top (or its middle when centering). Exact, on
                    // measured geometry — this used to be positionViewAtRow(),
                    // whose alignment ran through TableView's uniform average
                    // row height.
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

                // Which message currently has its action toolbar pinned open
                // (by a click). Shared across delegates so only one can be
                // pinned at a time; keyed by the SDK item id (or event id).
                property string pinnedActionsKey: ""
                // The row the pointer is currently over. Exactly ONE action
                // bar may be on screen: a pinned row keeps its bar only
                // while nothing else is hovered, otherwise hovering a second
                // row showed a second bar and both rows looked selected.
                property string hoveredActionsKey: ""
                property bool emojiPickerOpen: false
                // ── C6: ONE owner of transient row interaction ───────────
                // "" = nobody. Otherwise the name of the surface that owns it
                // right now: "picker" | "tone" | "menu" | "profile" |
                // "readers" | "viewer". While non-empty no row may show its
                // action bar and hover must not claim one.
                //
                // Confirmed defect: MessageDelegate's actionsVisible consulted
                // hoveredActionsKey / actionsPinned / moreMenuOpen and nothing
                // else, so the row toolbar kept rendering under an open picker
                // and its nested tone popup. Another boolean would have been a
                // fourth thing to keep in sync; one owner is the mechanism.
                // Never solved with z: raising the picker would only cover the
                // bar, and the bar is still hit-testable underneath it.
                property string transientInteractionOwner: ""
                function claimTransientInteraction(owner) {
                    if (!owner || owner.length === 0)
                        return
                    // CLEAR, not cover: the bar is gone, not merely hidden.
                    hoveredActionsKey = ""
                    pinnedActionsKey = ""
                    transientInteractionOwner = owner
                }
                // Release only what you own. The tone popup and its parent
                // picker close in an order the popups themselves decide, and
                // an unconditional release would let the CHILD's close hand
                // row interaction back while the picker is still on screen.
                function releaseTransientInteraction(owner, fallback) {
                    if (transientInteractionOwner !== owner)
                        return
                    transientInteractionOwner = fallback ? fallback : ""
                }
                // The hover guard lives HERE rather than in the delegate's
                // HoverHandler so that EVERY writer of the key is covered by
                // one rule — including a future one. Releasing ownership needs
                // no re-hover: the next real hover event sets the key again.
                onHoveredActionsKeyChanged: {
                    if (transientInteractionOwner !== ""
                        && hoveredActionsKey !== "")
                        hoveredActionsKey = ""
                }
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
                // The mutual-exclusion half of C6, reachable from a row.
                //
                // The reaction picker and a message context menu are both
                // Popup.Item in the same window overlay, so the one opened
                // LAST paints and hit-tests on top — and a menu opened over
                // an already-open picker covered the emoji grid entirely.
                // That is NOT solvable with z (see the C6 note above: raising
                // the picker only covers the action bar, which stays
                // hit-testable underneath). The two surfaces must be
                // mutually exclusive instead, and the closer has to live
                // HERE rather than on the pane root: delegates reach the
                // pane only through `timelineView`, so closeRowAnchoredSurfaces()
                // as a pane-root function is invisible to them — the exact
                // unreachability that silently swallowed every reader-list
                // click in the 2026-08-19 round.
                //
                // Contract: MessageDelegate.openContextMenu() calls this
                // BEFORE it assigns menuEventId, so opening a message menu
                // always dismisses an open picker, tone popup or profile
                // popover first.
                property var closeTransientRowSurfaces: function() {
                    root.closeRowAnchoredSurfaces()
                }
                property var openSenderProfile: function(member) {
                    senderProfilePopover.openFor(member)
                }
                // ── C5: the shared reply-navigation view contract ────────
                // MessageDelegate reaches its view ONLY through
                // `timelineView`, exactly as openSenderProfile and
                // openReactionPicker already do, so the SAME reply preview
                // works in the room and in the thread panel without either
                // knowing about the other's history loader.
                // PaginationController::jumpToEvent stays the single room
                // history loader; this adds no second one.
                property var navigateToEvent: function(eventId) {
                    if (!eventId || eventId.length === 0)
                        return
                    root.stopAutoscroll()
                    timeline.cancelWheelMotion()
                    app.pagination.jumpToEvent(eventId)
                }
                readonly property string navigationHighlightEventId:
                    app.pagination.highlightedEventId
                // 2026-08-19 fix: delegates reach the pane ONLY through
                // this Flickable (their `timelineView`), so the reader
                // list opener must live here — as a pane-root function it
                // was unreachable and the delegate's existence guard
                // silently swallowed every click.
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
                // reconciling. Positioning synchronously inside
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
                        positionViewAtLatest()
                }

                // TimelineScrollController supplies the configured mouse-
                // wheel speed and keyboard paging. The reversed model/layout
                // below removes the unstable prepend-before-visible-rows
                // operation that previously moved its coordinate frame.
                property bool wheelAnimating: app.timelineScroll.motionActive

                // ── Scroll-session state ─────────────────────────────────
                // True while the user's input owns the viewport: a native
                // drag/flick (moving/dragging), wheel/keyboard animation
                // (wheelAnimating), or direct touchpad deltas (the settle
                // timer). While this is true, Lightning must not write an
                // anchor correction into the input-owned position.
                readonly property bool userScrollActive:
                    moving || wheelAnimating || scrollSettleTimer.running

                // ── 2026-08-19: speculative media work waits for a settle
                //
                // A live capture (Rokas, 985-1026 loaded rows) showed ONE
                // 15-second upward gesture pull ~120 MB of video — 23, 13.5,
                // 12.6, 11.7, 9.5, 7.1, 6.5, 6.4, 6.1, 5.0, 4.8, 4.5, 3.9,
                // 3.0 and 2.8 MB payloads — because every row that merely
                // SWEPT THROUGH the on-screen band armed a full-payload
                // prefetch. Each completion then writes its temp file
                // synchronously on the GUI thread (writePlayableFile), and
                // the same capture logged unattributed GUI stalls of 333,
                // 369 and 1062 ms.
                //
                // Rows the reader never stopped on are not worth a
                // megabyte, so speculative work — full-payload prefetch and
                // the poster extraction that materializes one — waits until
                // the view settles. `userScrollActive` already includes the
                // 250 ms settle tail, so this resumes shortly after the
                // gesture ends and only for rows still on screen. THUMBNAILS
                // are deliberately not gated: they are small, they are what
                // the reader is actually looking at, and delaying them would
                // make scrolling look broken.
                readonly property bool speculativeMediaAllowed:
                    !userScrollActive

                // True only while a native drag/flick or a wheel/keyboard
                // ANIMATION owns contentY — i.e. while a structural change to
                // the row set would fight live input. Deliberately NOT
                // userScrollActive, which also counts the 250 ms settle TAIL
                // (scrollSettleTimer.running).
                //
                // That distinction was a shipped no-op, not a nicety:
                // applyRowWindow() runs from inside that same timer's
                // onTriggered, where `running` is still true, so guarding it
                // on userScrollActive meant the row window could NEVER apply.
                // A live capture showed frame work still scaling with total
                // loaded rows (27 ms at ~950 rows) with the window supposedly
                // active. Every offline test called applyRowWindow() directly,
                // where the property reads false — the policy was covered and
                // the TRIGGER was not.
                readonly property bool viewportMotionActive:
                    moving || wheelAnimating

                // True only while Lightning itself drives contentY. The
                // distinction is diagnostic only: NO active input path
                // receives an anchor write.
                readonly property bool selfDrivenScrollActive:
                    !moving && (wheelAnimating || scrollSettleTimer.running)

                // ── Bounded per-gesture scroll diagnostics ───────────────
                // Off unless LIGHTNING_SCROLL_TRACE is set (read once in
                // TimelineScrollController). When on, ONE summary line is
                // emitted per wheel/touchpad gesture at settle — never one per
                // event — so a physical tester can capture a real trace and
                // send it back. See the counter block below for what
                // anchorCorrections and growthCorrections each mean across
                // maintainViewAnchor()'s five distinct outcomes (no-anchor
                // return, displaced, fallback capture, drag deferral,
                // materialized delta / idle restore).
                // v0.7.2: a separate paginationRestores counter used to
                // live here for the pagination anchor's own restore path. It
                // is gone because that whole mechanism is gone — a prepend
                // landing mid-gesture is now counted by activeDeferrals and
                // absorbed into a measurement-only re-capture.
                // Its disappearance IS the evidence the two mechanisms
                // actually merged rather than one being renamed.
                // No message content, ids, or URLs are logged.
                readonly property bool scrollTrace: app.timelineScroll.scrollTraceEnabled
                property bool diagActive: false
                property int diagEvents: 0
                property int diagPixelEvents: 0
                property int diagAngleEvents: 0
                // diagAnchorCorrections counts ONLY the idle absolute-restore
                // path in maintainViewAnchor(). Under the carry-bucket
                // semantics (counters reset at print, not per gesture) it is
                // EXPECTED to be non-zero on ordinary traces: the idle path
                // runs between gestures — media hydration or late decryption
                // while the reader is parked mid-history — and those counts
                // carry into the next printed line. It can NOT run while a
                // gesture owns the view (the userScrollActive block returns
                // first), so it is no longer a fought-the-input red flag;
                // read it as "idle restores since the previous line".
                // diagGrowthCorrections counts the displaced-anchor fallback
                // that actually wrote contentY after input was already idle.
                // Active geometry deltas are instead recorded by the
                // activeDeferred* fields below and never applied.
                property int diagAnchorCorrections: 0
                property int diagGrowthCorrections: 0
                // Rows instantiated in the Column. There is no height cache
                // to count commits against any more: every row's height IS its
                // measured height, so the old heightCommits/heightCached/
                // heightCorr triple has nothing left to report.
                property int diagRowCount: 0
                // v0.7.x live report round 2 ("teleporty ... when there is
                // images ... scrolling up"): a proposed fix to the displaced-
                // anchor branch (bounding/symmetrizing its correction) was
                // reviewed and WITHDRAWN — the reviewer showed the premise
                // could not be confirmed from the existing single combined
                // diagGrowthCorrections counter, which cannot tell which of
                // the FIVE distinct outcomes in maintainViewAnchor() actually
                // ran — displaced, the capture-fallback, drag-deferral,
                // materialized, or the idle absolute restore — nor whether
                // the magnitude of any one correction was proportionate to
                // real inserted content. These per-outcome counters exist
                // ONLY to answer that empirically, from a real physical
                // gesture, before any behavior changes again. A second review
                // pass then found the counters below still had a structural
                // blind spot for exactly the scenario this exists to
                // diagnose, plus two smaller gaps; both are fixed here:
                //
                //   M1 (blind spot): diagFlushGesture() used to run as the
                //   LAST statement of scrollSettleTimer.onTriggered, i.e.
                //   while diagActive was still true — but a correction that
                //   lands async relative to settle (originally: the
                //   near-top backfill staging window's release-time flush,
                //   since removed outright — see the near-top backfill
                //   comment further down; the reasoning below is kept
                //   because the SAME async-loss risk still applies to any
                //   other post-settle correction, e.g. a media hydration or
                //   late-decryption height change landing a turn or two
                //   after settle) does NOT happen inside that handler, so a
                //   naive "print on the last statement of settle" misses it
                //   entirely: the correction fires after the line was
                //   already printed and diagActive already false. Instead,
                //   every outcome counter below (everything except the
                //   events/pixel/angle/netY/dContentH group, which describes
                //   THIS gesture's own physical input and has no async-loss
                //   risk) is now gated on `scrollTrace` alone, not
                //   `diagActive`, and is drained (printed, then zeroed) only
                //   at the point it is actually printed, not at the next
                //   gesture's first input. A correction landing in the gap
                //   between one flush and the next gesture's first delta is
                //   therefore carried forward and appears on the NEXT
                //   printed line rather than being silently dropped — never
                //   lost, at the cost of coarser attribution: a line's
                //   outcome counts are "since the previous line", which can
                //   include the tail of the gesture before it. For the
                //   maintainer's actual capture procedure (continuous
                //   scrolling) the gap is short and the attribution stays
                //   meaningful; a long pause between two unrelated gestures
                //   could carry a stale correction into an unrelated line.
                //   Read the counters as "what happened since the last
                //   line", not "what this gesture caused".
                //
                //   M2 (uninterpretable all-zero line): a line where every
                //   outcome counter is 0 does not distinguish "the mechanism
                //   ran and had nothing to correct" from "the mechanism
                //   never ran at all" (viewAnchorId empty, or stickToBottom).
                //   diagNoAnchorReturns and diagStickToBottomReturns below
                //   count those two causes separately (see the call site).
                //   READ THEM CAREFULLY: maintainViewAnchor() is never
                //   SCHEDULED while stickToBottom (both content/height
                //   handlers take the scrollToEndDeferred arm instead), so
                //   an ordinary bottom-pinned gesture moves NEITHER counter.
                //   A non-zero diagStickToBottomReturns means specifically
                //   that a correction was scheduled while scrolled up and
                //   then dropped because the reader returned to the bottom
                //   before the coalesced call fired — harmless in
                //   production (scrollToEndDeferred pins the view anyway),
                //   but it is a real signal, not routine noise. When BOTH
                //   conditions hold the empty-anchor arm wins, so an empty
                //   id while bottom-pinned reports as noAnchorReturns.
                //
                //   L1 (conflated fallback causes): the old single
                //   captureFallbacks counter merged two different causes of
                //   "the delegate isn't there" — a stable id that no longer
                //   resolves to any row at all (redaction, local-echo id
                //   swap: diagUnresolvedIdFallbacks) and a row that still
                //   resolves fine but whose delegate was simply evicted from
                //   the cache with NO proof any row was inserted above it
                //   (diagEvictedNoInsertFallbacks — expected to DOMINATE in
                //   a media-heavy room, since tall image rows push the
                //   cache window around on ordinary scrolling with no
                //   pagination involved at all). Distinguishing them is
                //   exactly what a real capture needs to tell "an insertion
                //   happened but couldn't be resolved" apart from "nothing
                //   was inserted, this is just eviction".
                //
                // displacedMaxAbsGrew and materializedMaxAbsDelta store the
                // SIGNED value at the sample with the largest ABSOLUTE
                // magnitude (selection by |x|, storage without abs()) — a
                // large negative outlier is not lost to Math.abs(), and the
                // printed sign tells you which direction the outlier swung.
                //
                // Numbers and a fixed branch label only — no message content,
                // ids, or URLs.
                property int diagNoAnchorReturns: 0
                property int diagStickToBottomReturns: 0
                property int diagDisplacedFirings: 0
                property real diagDisplacedAppliedSum: 0
                property real diagDisplacedMaxAbsGrew: 0
                property int diagDisplacedMaxAbsGrewRows: 0
                // Paired samples, deliberately selected in BOTH directions:
                // the origin shift on the largest-|grew| firing answers the
                // known skipped-negative case, while the content delta and
                // rows on the largest-|origin shift| firing reveal whether
                // originY has outliers of its own. Independent maxima without
                // these pairings would conflate two unrelated firings.
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
                // Wall clock actually spent handling ONE wheel event, called
                // by the handler around its own body. This is the number a
                // "scrolling locks up" report needs: row counts alone cannot
                // distinguish a timeline that is merely large from one whose
                // rows are individually expensive.
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
                // Drains (prints then zeroes) every outcome counter — see the
                // M1 comment above for why this is the ONLY place they reset,
                // rather than at the next gesture's first input.
                // Counts applyRowWindow() calls that actually changed the
                // window this gesture — zero next to a large rows/srcRows gap
                // is the signature of the no-op this round shipped.
                property int diagWindowApplications: 0
                // Counts newest-end window extensions taken DURING motion.
                // Zero next to a non-zero winSkip on a gesture that ran into
                // the bottom is the signature of the clamp this fixes.
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
                        // The row WINDOW's state, because the round that added
                        // it had none and shipped a permanent no-op unnoticed:
                        // `rows` above is what is INSTANTIATED and srcRows is
                        // what is loaded, so rows == srcRows with a deep reader
                        // means the window is not bounding anything. winSkip is
                        // how many of the newest rows are currently withheld.
                        + " srcRows=" + (app.timeline ? app.timeline.count : -1)
                        + " winSkip=" + rowWindowSkip
                        + " winApplies=" + diagWindowApplications
                        + " winExtendNew=" + diagWindowNewEndExtensions
                        // Non-zero names the row window as the thing that
                        // moved the reader: it corrected contentY using rows
                        // that still measured zero after a layout flush. The
                        // offscreen harness cannot reproduce that state, so a
                        // real capture is the only way to see it.
                        + " winUnmeasured=" + diagWindowUnmeasuredRows
                        // Cost, and what the loaded timeline is MADE of.
                        // gestureMs is wall clock across the whole gesture;
                        // worstNotchMs is the slowest single wheel event,
                        // which is what the user actually feels as a stall.
                        // stateRows/stateGroups say whether a large row count
                        // is mostly collapsed room activity — a run of 1000
                        // state rows drawing three summary lines is a very
                        // different defect from 1000 real messages.
                        + " gestureMs=" + Math.round(Date.now() - diagGestureStartMs)
                        + " worstNotchMs=" + Math.round(diagWorstNotchMs)
                        // -1 means "not available here", never zero: a
                        // fixture without a real TimelineModel must not be
                        // reported as a timeline containing no state rows.
                        // Guarded rather than assumed — throwing inside this
                        // string would abort the whole line, which is exactly
                        // the regression scrollTraceLineIncludesAllPerBranchFields
                        // exists to catch.
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
                // ── 2026-08-19 scroll round 2 part 3: the row window ─────
                //
                // Frame-time evidence from Rokas's GPU (QSG_RENDER_TIMING,
                // steady state with pagination frames EXCLUDED): median frame
                // 3 ms at ~108 loaded rows against 14 ms at ~916, and 1% vs
                // 46% of frames over the 16 ms budget; `render` grew 14x and
                // `polish` 5x. Bounding the instantiated rows is the only
                // lever that touches that (see CLAUDE.md — the earlier
                // offscreen numbers were a software-rasterizer artefact, but
                // this measurement is the real machine).
                //
                // SAFETY, and it is the whole design:
                //   * applied ONLY when the reader is settled — never a
                //     structural change mid-gesture, which is what sank the
                //     reverted bounded-retained-window;
                //   * a generous RUNWAY of rows is kept below the reader, so
                //     a real downward gesture cannot reach the window's low
                //     edge (the largest single downward gesture in the
                //     capture was ~7.5 viewports; the runway is ~30);
                //   * the low-end release is corrected by re-finding the
                //     reader's own anchor EVENT and restoring its screen
                //     offset — exact, not estimated, and bailing out before
                //     mutating anything if that anchor cannot be resolved;
                //   * atBottomEdge() refuses while the window hides the live
                //     edge, so follow-latest can never latch to a false
                //     bottom.
                readonly property int windowRunwayRows: 220   // below reader
                readonly property int windowMarginRows: 120   // above reader
                readonly property int windowMinRows: 320      // never below
                function applyRowWindow() {
                    if (!app.timelineView || !app.timelineView.setWindow)
                        return
                    // Settled only, and never against a timeline that is
                    // still growing or being navigated. viewportMotionActive,
                    // NOT userScrollActive: this function's only caller is the
                    // settle timer's own handler, where the settle timer still
                    // reads as running.
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
                    // Absolute offsets from the NEWEST source row: the
                    // proxy's skip plus the view row.
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
                    // Hysteresis: only move for a change worth a structural
                    // op, or the settle after every gesture would churn.
                    // EXEMPT wantSkip === 0. Closing to the live edge is
                    // always worth the op: with the reader parked at the
                    // window's synthetic bottom, updateVisibleRowRange()
                    // forces visibleFirstRow to 0, so wantSkip walks toward 0
                    // in steps — and the LAST fewer-than-40 rows could never
                    // be closed, leaving the newest messages permanently
                    // unreachable and atBottomEdge() permanently false
                    // (2026-08-20).
                    const rows = count
                    if (wantSkip !== 0
                        && Math.abs(wantSkip - skip) < 40
                        && Math.abs(wantRows - rows) < 40)
                        return
                    // THRASH GUARD. Releasing rows at the OLDEST end brings
                    // the reader closer to the new top, and inside
                    // nearTopEnterDistance (2.5 viewports) that dispatches a
                    // backfill which regrows exactly what was just released.
                    // Whether the margin clears that band is NOT a row-count
                    // question: the margin is a fixed number of rows while the
                    // band is 2.5 viewports, so it clears comfortably at 1244px
                    // and lands INSIDE at 2004px (a 4K client area). Decide it
                    // on MEASURED height: if trimming the tail would leave the
                    // reader inside the band, keep the tail and take only the
                    // skip change, which is where most of the win is anyway.
                    // NOTE the row numbering: these are CURRENT view rows,
                    // where the window we want starts at (wantSkip - skip).
                    // So the tail being released begins at
                    // (wantSkip - skip) + wantRows — using `wantRows` alone
                    // counts rows that are being KEPT and wildly overstates
                    // the release (it made this guard veto every trim).
                    const tailFirst = (wantSkip - skip) + wantRows
                    if (tailFirst < rows) {
                        let tailRelease = 0
                        for (let t = tailFirst; t < rows; ++t) {
                            const tailItem = itemAtViewRow(t)
                            if (tailItem)
                                tailRelease += tailItem.height
                        }
                        // Thresholded on the ENTER band, not the exit one.
                        // The exit distance is hysteresis for a reader who
                        // is already IN the band; what dispatches a backfill
                        // is crossing INTO it, so that is the line this has
                        // to stay clear of. Measured on the 900-row fixture:
                        // thresholding on exit (4043) suppressed a trim whose
                        // real outcome was 4018 — comfortably outside the
                        // 3110 enter band — and kept 666 rows where 503 were
                        // correct. A guard that over-fires spends exactly the
                        // rows this whole mechanism exists to release.
                        if (distanceFromTop() - tailRelease
                            <= nearTopEnterDistance)
                            wantRows = rows - (wantSkip - skip)
                    }
                    if (wantSkip === skip && wantRows === rows)
                        return
                    // ── The correction ──────────────────────────────
                    //
                    // Hold the READER'S OWN ROW at the same offset on
                    // screen, measured before and after. This replaced a
                    // per-row height sum, and a live capture is why:
                    //
                    //   winSkip 212 -> 300, contentH 35802 -> 21976,
                    //   topDist 2855 -> 17406
                    //
                    // The sum returned ~0 where the true shift was ~13826 px,
                    // and the reader was thrown the whole way. It has to:
                    // `itemAtViewRow()` over the released range asks for the
                    // NEWEST rows, which is the far end of the view from a
                    // reader parked in history — those delegates may be
                    // unmaterialised (the proxy's reveal is paced) or
                    // unmeasured, and either answers zero. Summing what is
                    // being taken away is guessing; measuring what is being
                    // KEPT is not.
                    //
                    // The anchor is resolved by stable id, so it survives the
                    // renumbering the skip change causes, and it is by
                    // definition on screen — the window is built around the
                    // visible range, so it is never in the released set.
                    //
                    // This is NOT the deferred snap this file warns about.
                    // That one ran through Qt.callLater, BEFORE the Column
                    // relaid out, so it read a stale y and clamped against
                    // new content. forceLayout() below is the positioner's
                    // synchronous flush: by the time the anchor is re-read,
                    // the geometry is the new geometry.
                    const anchorRow = Math.max(0, viewRowAtContentY(contentY))
                    const anchorItem = itemAtViewRow(anchorRow)
                    const anchorEventId = eventIdAtViewRow(anchorRow)
                    // A reader whose row cannot be resolved at all is in an
                    // incoherent view (mid-reset, nothing materialised), and
                    // applying a structural change there is how the reverted
                    // retained-window attempt dumped the reader. Bail.
                    if (!anchorItem || anchorEventId === "")
                        return
                    const anchorOffset = anchorItem.y - contentY

                    app.timelineView.setWindow(wantSkip, wantRows)
                    // Existence is not measurement: a row's height comes from
                    // its ColumnLayout, which only applies its implicit size
                    // from updatePolish(), so anything created in this turn
                    // reads zero until the positioner has run.
                    rowColumn.forceLayout()

                    const movedRow = viewRowForStableId(anchorEventId)
                    const movedItem = movedRow >= 0 ? itemAtViewRow(movedRow)
                                                    : null
                    if (movedItem) {
                        contentY = movedItem.y - anchorOffset
                    } else {
                        // The anchor did not survive, which should be
                        // impossible for an on-screen row. Count it rather
                        // than apply a correction computed from nothing.
                        ++diagWindowUnmeasuredRows
                    }
                    ++diagWindowApplications
                    updateStickAndPaginate()
                    captureViewAnchor()
                }

                // followStateApplies: pass FALSE for an input event the
                // geometry could not apply at all — a wheel notch into a bound
                // the position is already sitting on. Such an event moves
                // nothing, so it must leave follow-latest exactly as it found
                // it; letting it run the recompute meant one unappliable notch
                // in a room too short to scroll disengaged follow-latest and
                // raised a jump pill that no amount of further scrolling could
                // ever clear (2026-08-20). The near-top check still runs: a
                // reader pinned against the OLDEST loaded row is precisely who
                // wants more history, and that is the one bound where an
                // unappliable event is meaningful.
                function updateStickAndPaginate(followStateApplies) {
                    if (followStateApplies !== false)
                        stickToBottom = atBottomEdge()
                    // Active user scroll (wheel/pixel/keyboard): edge-latched so
                    // reaching the top re-arms the bounded backfill exactly once
                    // per approach, not on every settle.
                    checkNearTopEdge(true)
                }
                // Can an input event in this direction move the position at
                // all? towardsOlder is INCREASING contentY on this rotated
                // view. Evaluated BEFORE the motion is dispatched, because the
                // discrete-wheel engine clamps its own target and then reports
                // an active motion either way — there is nothing to read back
                // afterwards that distinguishes "moved" from "clamped".
                function wheelCanMove(towardsOlder) {
                    return towardsOlder ? contentY < wheelMaxY() - 0.5
                                        : contentY > wheelMinY() + 0.5
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
                // ── A1: the window's NEWEST edge gives rows back DURING
                // motion, not only at the 250 ms settle.
                //
                // Nothing used to lower windowSkip while the reader was
                // moving, so a sustained downward gesture was hard-clamped at
                // the window's SYNTHETIC newest edge — the reader hit what
                // looked like the bottom of the room and stopped there, with
                // the jump pill up, until they let go for a quarter of a
                // second. extendWindowAtNewEnd() performs ONE head insert and
                // returns false when the skip is already 0 (nothing to give),
                // which is also the honest answer to "is the bottom of this
                // view the bottom of the room".
                //
                // The chunk is deliberately modest: these rows are built
                // SYNCHRONOUSLY (unlike the old end, which rides the proxy's
                // paced reveal), so a 120-row restore would be a several
                // hundred millisecond stall at the documented 3-7 ms per row.
                // A clamp-and-stop is worse than a small hitch, and the
                // settle-time applyRowWindow() releases the surplus again.
                readonly property int windowNewEndExtendRows: 60
                // How close to the synthetic newest edge is close enough to
                // ask. One viewport of remaining runway, so the rows exist
                // before the reader arrives rather than after.
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
                    // Restored rows land at the HEAD, which pushes every kept
                    // row further from content y 0 by exactly their summed
                    // height. The proxy builds them synchronously and the
                    // Column has no inter-row spacing, so the sum IS exact —
                    // but only once they have been laid out. A row's height
                    // comes from its ColumnLayout, and a QQuickLayout applies
                    // its implicit size from updatePolish(), so a row created
                    // in this turn measures ZERO and the correction silently
                    // becomes ~0. See applyRowWindow(), which had the same
                    // bug and the same too-confident comment.
                    //
                    // Worse here than there: the glide handler re-enters this
                    // every frame while the position sits at the synthetic
                    // edge, so an uncorrected extension can repeat until the
                    // whole skip is consumed — a stall plus a jump to the
                    // live edge.
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
                        // TRANSLATE an in-flight glide rather than cancelling
                        // it: the reader asked for a distance, and cancelling
                        // would silently discard whatever was left of it. This
                        // is the call that exists for exactly this event (a
                        // prepend landing mid-glide) and it preserves the
                        // remaining distance by construction.
                        app.timelineScroll.translateActiveMotion(shift)
                    }
                    // ONE exact write and no deferred follow-up, for the same
                    // reason applyRowWindow() has none: a Qt.callLater snap by
                    // anchor id runs BEFORE the Column relayout, reads a stale
                    // y, and lands the reader somewhere they never asked to be.
                    ++diagWindowNewEndExtensions
                    return true
                }

                // True when the window was holding rows back and has now
                // been asked to release more of them. Rows older than the
                // window's oldest exposed row are ALREADY loaded; the window
                // is the only reason they are not on screen.
                function extendRowWindowAtOldEnd() {
                    if (!app.timelineView
                        || !app.timelineView.extendWindowAtOldEnd)
                        return false
                    // The proxy decides, because only it can tell the WINDOW's
                    // cap apart from the pacing backlog. Re-deriving it here
                    // as `rowWindowSkip + count < total` was wrong: that is
                    // also true while the initial paced reveal is in flight
                    // with no window at all, and it swallowed the near-top
                    // request on every ordinary timeline
                    // (nearTopProximityIsMeasuredFromLoadedHistoryNot
                    // AbsoluteContentY caught it).
                    return app.timelineView.extendWindowAtOldEnd(
                               windowMarginRows) === true
                }

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
                // Keep multiple viewports of loaded runway ahead of an
                // aggressive upward wheel gesture. Waiting until half a
                // viewport remained guaranteed that a fast mouse reached the
                // old hard bound before one 100ms SDK poll could land. Earlier
                // prefetch keeps position ownership with the user's live input;
                // unlike replaying overscroll after a load, it never skips a
                // page the reader has not seen.
                readonly property real nearTopEnterDistance: height * 2.5
                readonly property real nearTopExitDistance: height * 3.25
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
                function distanceFromTop() { return wheelMaxY() - contentY }
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
                            // LOCAL ROWS FIRST. With a window active the
                            // reader can reach its oldest exposed row, where
                            // atYBeginning goes true and this would ask the
                            // homeserver for history that is already in the
                            // source model, merely not exposed — blocking the
                            // reader at a boundary the window itself created.
                            // Re-expose instead. Deliberately does NOT consume
                            // nearTopArmed: no network request is made and
                            // nothing at the head moves, so there is no
                            // per-approach budget to spend, and consuming it
                            // would stall the reader at the next edge until
                            // the 250ms settle. The extension is PACED by the
                            // proxy (3ms/tick at the tail), so it cannot
                            // become a synchronous 120-row build, and the
                            // settle-time applyRowWindow() re-trims behind
                            // the reader.
                            if (extendRowWindowAtOldEnd())
                                return
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
                    // Any cancellation retires a pending follow-latest
                    // arrival: the motion that would have delivered it is
                    // gone, so honouring it later would yank a reader who
                    // has since taken over.
                    followLatestOnArrival = false
                    app.timelineScroll.cancel()
                }

                // ── 2026-08-19: jump-to-latest glides instead of teleporting
                // NEAR the bottom, and stays instant beyond that.
                //
                // The engine is the SAME coalescing exponential-approach
                // motion the mouse wheel and PageUp/PageDown already drive
                // (app.timelineScroll.animateTo) — no new animation
                // mechanism, and every scroll-session guard in this file
                // (userScrollActive, scrollToEndDeferred's !wheelAnimating,
                // the anchor write-suppression) already accounts for a
                // motion being in flight.
                //
                // Beyond the threshold it stays a jump, deliberately: at the
                // engine's half-a-viewport-per-frame ceiling a
                // twenty-viewport slide is a second-long blur, not motion.
                // Element does not animate this at ALL — ScrollPanel's
                // scrollToBottom() is a bare `scrollTop = scrollHeight`, and
                // when the reader is far back TimelinePanel.jumpToLiveTimeline()
                // does not scroll through the backlog either: it rebuilds the
                // timeline at the live edge and drops what was paginated.
                property int smoothJumpViewports: 4
                property bool followLatestOnArrival: false
                onWheelAnimatingChanged: {
                    if (wheelAnimating || !followLatestOnArrival)
                        return
                    followLatestOnArrival = false
                    // Self-guarding: if the reader redirected mid-glide and
                    // ended somewhere else, that intent wins — never pin.
                    if (!atBottomEdge())
                        return
                    settleAtLatest()
                }
                // The follow-latest bookkeeping, shared by both paths.
                function settleAtLatest() {
                    // Addressing the live edge requires the live edge to be
                    // exposed — the same invariant the jump paths carry (see
                    // releasePendingRows). positionViewAtLatest() is a pure
                    // geometry write to wheelMinY(), which under an active
                    // window is the window's newest row, NOT the newest
                    // message. This is the fallback landing when the history
                    // trim refuses, so it must not depend on that trim's
                    // model reset having cleared the window for it.
                    releasePendingRows()
                    stickToBottom = true
                    // positioning row zero can re-seed the position frame
                    // from an estimate; a surviving anchor baseline would
                    // then measure a delta across two different frames.
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
                    // Keyboard/programmatic motion is another owner of
                    // contentY: end an autoscroll gesture rather than letting
                    // the two write it on alternate frames.
                    root.stopAutoscroll()
                    // Clamp defensively so keyboard callers (which pass an
                    // unclamped target) and any rounding can never drive an
                    // out-of-range or jittering contentY.
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    targetY = targetY < lo ? lo : (targetY > hi ? hi : targetY)
                    app.timelineScroll.animateTo(targetY, contentY, lo, hi,
                                                 height)
                    updateStickAndPaginate()
                    // Any upward intent leaves follow-latest — applied last so
                    // the geometry recompute above (still on the pre-motion
                    // position) cannot re-enable it while scrolling up.
                    if (targetY > contentY + 0.5) {
                        stickToBottom = false
                        // ...and retires a pending follow-latest arrival, the
                        // same way the wheel-up notch does. animateTo() on an
                        // ALREADY-active motion does not re-toggle
                        // motionActive, so a keyboard redirect mid-glide fires
                        // no arrival handler at the interrupt — without this
                        // the stale flag could later fire settleAtLatest()
                        // just because the keys happened to land inside the
                        // bottom slack band (review find).
                        followLatestOnArrival = false
                    }
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
                    // Paging is the reader driving, exactly as the wheel is —
                    // a keyboard-only reader was still being teleported. This
                    // belongs here rather than in beginWheelTo(), which
                    // goToLatest()/goToEarliestLoaded() also use and which a
                    // future programmatic caller could reach; all three
                    // callers of keyboardPage are Keys handlers.
                    noteReaderTookControl()
                    // The table is rotated: increasing logical contentY moves
                    // physically upward toward older rows.
                    beginWheelTo(contentY - direction * height * 0.9)
                }
                // Home is programmatic navigation like End: it bypasses the
                // wheel motion engine and jumps directly, then recomputes
                // pagination / follow-latest and saves one settled anchor.
                function goToEarliestLoaded() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    contentY = wheelMaxY()
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
                function goToLatest() {
                    root.stopAutoscroll()
                    cancelWheelMotion()
                    // Rotated view: the newest row sits at wheelMinY(), so
                    // the distance home is contentY - wheelMinY().
                    var lo = wheelMinY()
                    var distance = contentY - lo
                    // With a row window active, wheelMinY() is the window's
                    // SYNTHETIC newest edge, not the live edge — the newest
                    // rows are not exposed at all. Gliding there would land
                    // on a message that is not the latest and, because
                    // atBottomEdge() correctly still reports false, leave the
                    // jump pill on screen demanding a second press (review
                    // finding). A reader carrying a window is by definition
                    // deep in history, which is exactly the FAR case below,
                    // so refuse the glide and let the trim/jump path restore
                    // the live edge for real.
                    if (height > 0 && distance > 0 && rowWindowSkip === 0
                        && distance <= height * smoothJumpViewports) {
                        followLatestOnArrival = true
                        app.timelineScroll.animateTo(lo, contentY, lo,
                                                     wheelMaxY(), height)
                        // The settle pass recomputes pagination/anchoring for
                        // the arrival exactly as it does for a wheel gesture.
                        scrollSettleTimer.restart()
                        return
                    }
                    // FAR: beyond the glide threshold. Element's own answer to
                    // this case is not a faster scroll — it is to stop
                    // carrying the backlog at all: jumpToLiveTimeline()
                    // rebuilds the timeline at the live edge and DISCARDS
                    // everything paginated. Do the same, and only here: the
                    // trim is an explicit user action, never a side effect of
                    // scrolling. It refuses on its own (wrong backend, no
                    // room, mid-pagination, too few rows to be worth a reset)
                    // and then this falls through to the ordinary jump.
                    //
                    // No anchor work is needed for it, which is exactly why
                    // this is the safe place to do it: the reader ends up
                    // pinned at the newest row, where there is no scroll
                    // position to preserve. onModelReset() handles the
                    // landing — the same path a room open already uses.
                    // Only commit to the trim when the dispatch actually
                    // succeeded. It returns false for a refusal AND for a
                    // failed send, and in the latter case no reset will ever
                    // arrive — persisting follow-latest there would teleport
                    // the reader on the next live message while they are
                    // still mid-history (review finding).
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

                // TimelineScrollController drives mouse-wheel and keyboard
                // motion. QML applies each frame against the live bounds.
                Connections {
                    target: app.timelineScroll
                    function onWheelPositionChanged(y) {
                        var lo = timeline.wheelMinY()
                        // A GLIDE can run into the window's synthetic newest
                        // edge too — the wheel handler only extends per event,
                        // and one large notch outlives its own event. Give the
                        // rows back instead of settling at a bottom that is not
                        // the bottom.
                        if (y <= lo && timeline.rowWindowSkip > 0
                            && timeline.extendRowWindowAtNewEnd()) {
                            // The extension already moved contentY and
                            // translated the in-flight motion by exactly the
                            // restored height, so THIS frame's y is stale by
                            // that amount. Drop it; the engine's next frame
                            // carries the corrected position.
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
                        // Re-evaluate the row window now that the reader has
                        // settled. This is the ONLY place it is applied — a
                        // structural change mid-gesture is what sank the
                        // reverted bounded-retained-window (2026-08-19).
                        timeline.applyRowWindow()
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
                        // The single wheel-gesture settle point: emit one
                        // bounded diagnostic summary.
                        timeline.diagFlushGesture()
                    }
                }

                WheelHandler {
                    id: timelineWheelHandler
                    objectName: "timelineWheelHandler"
                    // This handler is the single pointer-wheel owner. The
                    // reversed proxy plus rotated table ensures loading older
                    // history extends the far edge instead of moving every
                    // visible row underneath that owner.
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        // Timed only while LIGHTNING_SCROLL_TRACE is set;
                        // Date.now() is not called at all otherwise.
                        var notchStartMs = timeline.scrollTrace ? Date.now() : 0
                        // A wheel notch is another owner of contentY; an
                        // autoscroll gesture still running would write it on
                        // alternate frames.
                        root.stopAutoscroll()
                        // ...and so is a jump that has not landed yet. The
                        // reader turning the wheel is an explicit "I am
                        // driving now"; without this the pending landing
                        // survives the whole gesture and teleports the view
                        // back the moment its target becomes measurable.
                        timeline.noteReaderTookControl()
                        // Positive delta on either axis is the OLDER
                        // direction on this rotated view (see wheelTargetY /
                        // pixelTargetY, which both take the negated value).
                        var towardsOlder = event.pixelDelta.y !== 0
                                ? event.pixelDelta.y > 0
                                : event.angleDelta.y > 0
                        // Give the window its newest rows back BEFORE the
                        // motion is computed, so the notch clamps against the
                        // extended geometry instead of the synthetic edge.
                        if (!towardsOlder && timeline.nearWindowNewEdge())
                            timeline.extendRowWindowAtNewEnd()
                        // Evaluated before dispatch: see wheelCanMove().
                        var canMove = timeline.wheelCanMove(towardsOlder)
                        var minY = timeline.wheelMinY()
                        var maxY = timeline.wheelMaxY()
                        if (event.pixelDelta.y !== 0) {
                            timeline.cancelWheelMotion()
                            timeline.contentY = app.timelineScroll.pixelTargetY(
                                -event.pixelDelta.y, timeline.contentY,
                                minY, maxY)
                            timeline.updateStickAndPaginate(canMove)
                            if (event.pixelDelta.y > 0 && canMove)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0
                                   && !timeline.smoothScrollingEnabled) {
                            // Smooth scrolling OFF: land the notch at once.
                            // The DISTANCE is unchanged — notchDistance() is
                            // the same value the glide integrates toward and
                            // it honours the configured wheel speed, so this
                            // is the same travel without the animation, not a
                            // different scroll speed.
                            //
                            // notchDistance() is the ONLY stateless call on
                            // this controller: wheelTargetY() and
                            // pixelTargetY() both MUTATE its single shared
                            // motion state as a side effect, which is why the
                            // pixel branch above cancels first and why this
                            // one must not borrow either of them.
                            timeline.cancelWheelMotion()
                            var per = app.timelineScroll.notchDistance(
                                timeline.height)
                            var jump = -(event.angleDelta.y / 120.0) * per
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
                                // Wheeling UP mid-glide is an explicit
                                // "not to the bottom" — retire the pending
                                // follow-latest arrival rather than leaning
                                // on the arrival guard alone.
                                timeline.followLatestOnArrival = false
                            }
                            timeline.diagNoteEvent(false)
                            scrollSettleTimer.restart()
                        }
                        timeline.diagNoteNotchCost(notchStartMs)
                        event.accepted = true
                    }
                }

                // A native drag or flick takes ownership of contentY, and a
                // glide still in flight would fight it — the same interlock
                // the scrollbar and middle-click autoscroll already use. Only
                // drag/flick (never a programmatic write) reaches these.
                onDragStarted: { cancelWheelMotion(); noteReaderTookControl() }
                onFlickStarted: { cancelWheelMotion(); noteReaderTookControl() }

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
                // The binding above cannot be true until the view has
                // settled, and opening a room subscribes it in sliding sync
                // — so the room's own recent history arrives as live appends
                // during exactly the window where nothing suppresses it, and
                // the room being read notifies for every message it loads.
                Binding {
                    target: app
                    property: "activeRoomHydrating"
                    value: timeline.visible && !timeline.presentationReady
                           && timeline.Window.active === true
                }

                // v0.6.6: TimelineModel's near-top backfill "virtual
                // scrolling" staging window — which used to hold a landed
                // batch out of the exposed row space entirely while a
                // gesture was held through an unsettled near-top approach —
                // was removed outright. With the chain-continuation defect
                // fixed (finishBatch() now ends a near-top run at the FIRST
                // productive page instead of auto-chaining while staged —
                // see PaginationController::finishBatch()), staging no
                // longer had multiple batches to coalesce: it held at most
                // one bounded page, and for that single page it cost a real
                // wall — the loaded page sat invisible and contentY could
                // not advance until the gesture physically ended, the
                // opposite of the maintainer's actual ask ("when content
                // loads actually allow normal scrolling again"). A live
                // bottom-append landing during that window could also be
                // misclassified as the run's own progress, ending it having
                // delivered no history at all, with no way to ask again
                // while frozen. Every landed batch is now an ordinary
                // immediate prepend again — exactly the un-staged path this
                // file already used for ViewportFill/Retry — compensated by
                // the same maintainViewAnchor() mechanism below as soon as
                // it lands, never deferred to gesture-end.

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
                // ── The fill loop is SELF-HEALING, not geometry-triggered ──
                //
                // maybeFillViewport() is level-triggered and armed ONLY by
                // geometry signals (onContentHeightChanged, onHeightChanged,
                // Component.onCompleted, the deferred onModelReset call, the
                // room-activity toggle). Two ordinary things leave it with no
                // trigger left, and the room is then permanently unscrollable:
                //
                //   * PaginationController::request() drops a dispatch with no
                //     retry armed while a request is already in flight (its
                //     m_deferredFill recovery covers only the not-ready case);
                //   * a batch can insert rows the timeline does not RENDER —
                //     routine room activity while showRoomActivity is off is
                //     zero-height — so contentHeight never moves and no
                //     geometry signal fires.
                //
                // With contentHeight + margins <= height, wheelMaxY() equals
                // wheelMinY() and every input path is a no-op by construction:
                // the reported "a freshly opened room cannot be scrolled until
                // I resize the window", where the resize is literally the user
                // re-running this function.
                //
                // The retry is bounded TWICE, and neither bound alone would
                // do. This counter caps consecutive re-arms and only resets
                // when the room resets or the viewport is genuinely filled —
                // needed because PaginationController's own budget is NOT
                // spent by a dropped dispatch. And the controller's budget
                // (m_maxFillRequests, plus kMaxNoProgressStrikes) latches
                // `fillStopped`, which is checked below — needed because this
                // counter alone would not stop a backend that keeps returning
                // rows the timeline cannot show. reachedStart / failed end it
                // too. So the loop cannot spin: every path either grows the
                // content, exhausts a bound, or ends the room.
                readonly property int maxViewportFillRetries: 8
                /// Pages that added ROWS but no visible height. Generous,
                /// because each one advances the pagination cursor through a
                /// collapsed run towards the real messages beyond it — the
                /// reader must never have to expand an activity group to reach
                /// older history. Still bounded: an enormous room stops here
                /// rather than paginating to its start.
                readonly property int maxInvisibleFillRetries: 60
                property int viewportFillInvisibleRetries: 0
                property int viewportFillRetries: 0
                /// contentHeight at the previous fill attempt, or -1 for "no
                /// attempt yet". The budget is spent on attempts that did not
                /// make the content taller, not on attempts.
                property real viewportFillLastHeight: -1
                /// Loaded row count at the previous attempt, or -1 for none.
                property int viewportFillLastRows: -1
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
                            viewportFillLastHeight = -1
                            viewportFillLastRows = -1
                            viewportFillRetryTimer.stop()
                            return
                        }
                        // NOTHING IS DECIDED WHILE A PAGE IS IN FLIGHT.
                        //
                        // The budget counts PAGES that failed to help. This
                        // function is called by every geometry signal, so
                        // several calls land between one request and its
                        // completion — the trace shows "duplicates suppressed
                        // count= 4" — and each of those saw no growth for the
                        // trivial reason that the page had not arrived yet.
                        //
                        // Counting them spent the whole budget on redundant
                        // calls: measured, an archived room reported "fill
                        // budget exhausted requests= 8" while its rows had
                        // gone 79 -> 123, so pages WERE productive and the
                        // generous bound for invisible progress was never
                        // reached. The retry timer is re-armed so a page that
                        // lands without moving any geometry still wakes this
                        // up, which is the deadlock the timer exists for.
                        if (app.pagination.busy) {
                            viewportFillRetryTimer.restart()
                            return
                        }
                        // WHY A FILL DECLINED, named. Three rounds of this
                        // defect were spent reasoning about which bound had
                        // stopped the loop, and the answer was a bound in
                        // PaginationController that the QML side cannot see.
                        // One line settles it instead of a fourth hypothesis.
                        function declineReason() {
                            if (app.pagination.reachedStart) return "reachedStart"
                            if (app.pagination.fillStopped) return "fillStopped"
                            if (app.pagination.failed) return "failed"
                            if (viewportFillRetries >= maxViewportFillRetries)
                                return "noProgressBudget"
                            if (viewportFillInvisibleRetries
                                >= maxInvisibleFillRetries)
                                return "invisibleBudget"
                            return ""
                        }
                        // TWO KINDS OF PROGRESS, AND ONLY ONE OF THEM IS
                        // VISIBLE.
                        //
                        // Captured 2026-08-31 in an archived room whose whole
                        // tail is routine state: rows=205, stateRows=184,
                        // stateGroups=1, contentH=60. A hundred and
                        // eighty-four state rows fold into ONE collapsed
                        // group, so contentHeight is 60px and can never reach
                        // `height` — the guard above is unsatisfiable.
                        //
                        // The first bound here counted attempts and issued the
                        // request BEFORE checking, so exhausting it stopped
                        // only the retry TIMER while every geometry signal —
                        // and each of the hundreds of paced row reveals is one
                        // — went on issuing another request. That was the
                        // freeze.
                        //
                        // Bounding it on HEIGHT then traded the freeze for the
                        // original complaint: the room stopped after eight
                        // pages and the reader had to expand the activity
                        // group by hand before anything more would load.
                        // Expanding must never be required to reach older
                        // messages.
                        //
                        // So a page that added ROWS is progress even when it
                        // added no pixels: the pagination cursor moved, and it
                        // moved towards the real messages beyond the collapsed
                        // run. It gets the generous bound. A page that added
                        // NOTHING moved nothing and gets the small one. Both
                        // terminate, and the request is gated by the check, so
                        // there is one request per completed page rather than
                        // one per geometry signal.
                        var loadedRows = app.timeline ? app.timeline.count : 0
                        var grewHeight = viewportFillLastHeight >= 0
                                && contentHeight > viewportFillLastHeight + 1
                        var grewRows = viewportFillLastRows >= 0
                                && loadedRows > viewportFillLastRows
                        if (grewHeight) {
                            // Ordinary filling: the reader is gaining visible
                            // history, so the budget is not being spent at all.
                            viewportFillRetries = 0
                            viewportFillInvisibleRetries = 0
                        } else if (grewRows) {
                            // Invisible progress: keep going, but not forever.
                            viewportFillRetries = 0
                            ++viewportFillInvisibleRetries
                        }
                        viewportFillLastHeight = contentHeight
                        viewportFillLastRows = loadedRows
                        var decline = declineReason()
                        if (decline !== "") {
                            if (scrollTrace) {
                                console.log("fill-declined reason=" + decline
                                    + " rows=" + loadedRows
                                    + " contentH=" + Math.round(contentHeight)
                                    + " height=" + Math.round(height)
                                    + " noProgress=" + viewportFillRetries
                                    + " invisible=" + viewportFillInvisibleRetries)
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
                                + " noProgress=" + viewportFillRetries
                                + " invisible=" + viewportFillInvisibleRetries)
                        }
                        viewportFillRetryTimer.restart()
                    })
                }
                // Content height changes whenever any delegate's height
                // settles (text measurement, media hydration, link preview,
                // decryption replacement). One coalesced reaction per batch:
                // keep the newest event pinned while following the bottom,
                // otherwise hold the reader's anchor steady.
                onContentHeightChanged: {
                    // Any Column relayout after the reset reflects the new
                    // model's delegates (old ones are gone once anything
                    // moves), so contentHeight is meaningful again.
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
                // NOTE: no onWidthChanged invalidation. Wrapped text heights
                // used to be cached per row and had to be thrown away when the
                // wrapping constraint changed. Rows now re-wrap and re-measure
                // themselves, and the Column re-lays out from the result.

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
                    function onPaginationCompleted(insertedCount, reachedStart,
                                                   willContinue) {
                        if (insertedCount <= 0 || reachedStart || willContinue)
                            return
                        // A productive page relocated the history edge. Start a
                        // fresh distance ratchet from that new edge so continued
                        // upward motion can prefetch the following page before
                        // exhausting the newly loaded runway. No position write.
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance =
                                timeline.distanceFromTop()
                    }
                    function onTargetLocated(row, pixelOffset, highlight) {
                        // `highlight` distinguishes a REPLY jump (true) from
                        // a scroll-anchor RESTORE (false), and the gate below
                        // is about which of them may still land AFTER the
                        // reader has taken the view.
                        //
                        // Be clear about what this does NOT do: a deliberate
                        // gesture cancels EVERY pending jump, reply included,
                        // through noteReaderTookControl(). That is intended.
                        // A reply jump that has to paginate can take seconds,
                        // and landing it mid-gesture is the same teleport the
                        // round exists to remove — the reader's hands beat a
                        // click they have already scrolled away from.
                        //
                        // What the gate adds is the case the cancel cannot
                        // reach: the reader scrolls, and a RESTORE that was
                        // already in flight resolves just afterwards. Nobody
                        // asked for that one, so it is dropped. A reply jump
                        // issued after a gesture still lands normally —
                        // readerControlledSinceReset only ever blocks the
                        // non-highlight case.
                        if (!highlight && timeline.readerControlledSinceReset) {
                            ++timeline.diagNavigationAbandoned
                            return
                        }
                        // Reply navigation takes control immediately.
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
                        Qt.callLater(function() {
                            // The target can be older than what the proxy has
                            // paced out so far, or hidden behind the row
                            // window; without this the jump resolves to -1 and
                            // nothing happens at all.
                            timeline.releasePendingRows()
                            // C5b/B1: see beginNavigationLanding(). Landing
                            // in THIS turn is what made every jump to a target
                            // that was not already exposed a silent no-op.
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
                    // Unconditional: the activation range must track the
                    // viewport even for programmatic moves.
                    scheduleVisibleRowRange()
                    // React to native drag/flick/wheel movement and keyboard
                    // paging, but ignore unrelated programmatic navigation.
                    // A native wheel does not need to expose `moving`: the
                    // passive observer above keeps the settle timer active for
                    // the duration of its event stream.
                    if (!userScrollActive) return
                    stickToBottom = atBottomEdge()
                    // Trigger backfill as the user approaches the top
                    // (drag/flick/wheel), edge-latched with hysteresis so it
                    // fires once per approach rather than every frame.
                    checkNearTopEdge(true)
                }
                onCountChanged: {
                    scheduleVisibleRowRange()
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
                    // Captured BEFORE the reset dispatch reaches the proxy/
                    // Repeater: were there any old rows whose delegates
                    // could linger and leave contentHeight reading the
                    // OUTGOING content after the reset? When the previous
                    // state was empty (first room of the session), nothing
                    // can linger, the Column's rebuild is the only
                    // geometry, and arming the staleness gate would only
                    // delay the fast fillsViewport open until the settled/
                    // guard fallbacks (review 2026-08-18).
                    function onModelAboutToBeReset() {
                        timeline.presentationResetHadRows = timeline.count > 0
                    }
                    function onModelReset() {
                        // A room switch / fresh snapshot must cancel any
                        // in-flight wheel motion from the previous room — and
                        // any autoscroll gesture, which survives a reset with
                        // nothing left to scroll.
                        timeline.cancelWheelMotion()
                        root.stopAutoscroll()
                        // Any reset discards the rows these surfaces were
                        // opened from — including a same-room jump-to-live
                        // trim, which changes no room id and so fires none
                        // of the switch-driven cleanup.
                        root.closeRowAnchoredSurfaces()
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
                        // A fresh room gets a fresh fill-retry budget; the
                        // previous room's spent attempts must not deny this
                        // one its own.
                        timeline.viewportFillRetries = 0
                        timeline.viewportFillInvisibleRetries = 0
                        timeline.viewportFillLastHeight = -1
                        timeline.viewportFillLastRows = -1
                        viewportFillRetryTimer.stop()
                        // A pending navigation landing belongs to the
                        // snapshot it was resolved against.
                        timeline.navigationPendingRow = -1
                        timeline.navigationPendingId = ""
                        navigationLandingTimer.stop()
                        // A fresh room is one the reader has not taken a
                        // position in yet, so its anchor restore is welcome.
                        timeline.readerControlledSinceReset = false
                        // Re-engage the presentation gate for the fresh
                        // snapshot. Recompute only after this whole signal
                        // dispatch settles: the pagination controller's own
                        // reset slot may run after this handler, and reading
                        // its previous room's settled state here could open
                        // the gate on a one-item partial snapshot. A warm
                        // cache that already fills the viewport re-opens the
                        // gate on the same deferred turn (sub-frame).
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

                // Pagination trigger: scroll to top with backfill available.
                // Duplicate and reached-start suppression live in the
                // controller.
                //
                // Deliberately NOT edge-latched through checkNearTopEdge()/
                // nearTopArmed the way the active drag/wheel path is: that
                // latch early-returns whole while stickToBottom is true (see
                // its own body), which would silently swallow the fresh-room
                // initial-history kick-off — a just-opened room starts
                // stickToBottom=true with too little content to scroll, and
                // this passive atYBeginning edge is the ONLY trigger for that
                // first fill. Reusing the active latch here would need to
                // special-case stickToBottom inside it, which risks the
                // active path's own precisely-tested hysteresis (many
                // existing tests pin nearTopArmed's exact enter/exit/re-arm
                // behavior) for a passive signal that already has its own,
                // different bound:
                //   * onAtYBeginningChanged only re-fires on an actual
                //     false->true transition of Qt's own atYBeginning
                //     property — never a per-frame poll — so a dispatch here
                //     always costs at least one real property-change event,
                //     not a tight loop;
                //   * userInitiated=false, so it can never RESET the
                //     controller's empty-strike counter (only a genuine user
                //     gesture does) — a run of filtered/empty pages this path
                //     alone triggers still latches at kMaxNearTopEmptyStrikes,
                //     identically to the active path;
                //   * PaginationController::request()'s single-flight makes a
                //     same-turn re-fire (e.g. two toggles inside one
                //     coalesced Qt.callLater window) a harmless "duplicate
                //     suppressed" no-op, logged and otherwise inert;
                //   * a PRODUCTIVE page (the remaining risk: a genuinely
                //     resettable-to-true atYBeginning caused by a ListView
                //     content-height ESTIMATE settling back to originY after
                //     a prepend, not real user motion) still ends the run
                //     immediately per finishBatch() — one page, then nothing
                //     further dispatches until atYBeginning genuinely toggles
                //     false and true again, each occurrence separately paced
                //     by a real network round trip. Not literally unbounded
                //     in the sense the withdrawn M5 staging chain was (no
                //     bound at all beyond exhausting the room); bounded by
                //     the union of the above, at the cost of a possible extra
                //     page or two if that estimate-churn scenario recurs —
                //     accepted rather than risking the active path's tested
                //     invariants for an unconfirmed, narrower edge case.
                onAtYEndChanged: {
                    if (atYEnd)
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

                // NO attached ScrollBar here. This Flickable is rotated 180
                // degrees, so an attached vertical bar renders on the visual
                // LEFT and its handle travels backwards. The bar is declared
                // as an UNROTATED sibling below and mapped explicitly.
                //
                // For the same reason there is no empty-state label in here
                // either: a Flickable reparents its visual children into the
                // rotated contentItem, so the old centred "No messages yet"
                // Label rendered UPSIDE DOWN. It now lives with the loading
                // surface and the jump pill, which are siblings of this
                // Flickable precisely so they read the right way up.
            }

            // Timeline scrollbar, deliberately OUTSIDE the rotated Flickable
            // so it sits on the right and travels the right way. The view is
            // rotated, so the visual TOP of the content is wheelMaxY (the
            // oldest loaded row) and the visual BOTTOM is wheelMinY (the
            // newest) — the mapping below is that inversion, in both
            // directions so dragging still works.
            //
            // AppScrollBar only restyles the handle and groove — every
            // property this block relies on (size, position, pressed, the
            // AsNeeded auto-fade) is ScrollBar's own, so the inversion and
            // the imperative-position Binding below are untouched by it.
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
                // The tracking binding lives in the Binding below, NOT here:
                // ScrollBar assigns `position` imperatively while the handle
                // is dragged, and a plain declarative binding on it would be
                // destroyed by that first assignment and never restored —
                // the bar would follow the timeline until the user touched
                // it once and then go dead.
                onPositionChanged: {
                    // Only follow the handle while the USER holds it (a
                    // groove click presses too). Otherwise this would fight
                    // the binding and the timeline's own motion.
                    if (!pressed)
                        return
                    var frac = (1 - size) > 0 ? position / (1 - size) : 0
                    timeline.cancelWheelMotion()
                    // Dragging the handle is the reader driving the view, so
                    // it retires an unlanded jump exactly as the wheel does.
                    timeline.noteReaderTookControl()
                    timeline.contentY = timeline.wheelMaxY() - frac * span
                    timeline.updateStickAndPaginate()
                }
            }

            Binding {
                target: timelineScrollBar
                property: "position"
                // Yields for the duration of the drag, then takes over again.
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
                        // v0.6.5: an inline text link — reads AppTheme.link
                        // (periwinkle under Storm), not accent (bolt,
                        // reserved for selection/focus/the one primary
                        // action), consistent with every other inline
                        // "Retry"/action link in the timeline.
                        //
                        // An AbstractButton, not a Label with a MouseArea on
                        // it: as a bare label the ONLY way to retry a failed
                        // backfill was a mouse click — no focus, no Space or
                        // Return, nothing in the accessibility tree.
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
                            // Cursor only — a MouseArea that accepted buttons
                            // here would swallow the button's own clicks.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                cursorShape: Qt.PointingHandCursor
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
                onNewMessageRequested: root.newConversationRequested("dm", undefined)
                onCreateRoomRequested: root.newConversationRequested("room", undefined)
                onCreateSpaceRequested: root.newConversationRequested("space", undefined)
            }

            // 2026-08-18 tester report ("still no middle click scrol"):
            // desktop autoscroll over the timeline. It is a SIBLING of the
            // Flickable, not a child: `timeline` is rotated 180 degrees, so
            // anything inside it would receive mirrored coordinates. It
            // therefore drives contentY through the same wheelMinY/wheelMaxY
            // bounds the wheel handler uses, with `inverted` set because a
            // rotated view scrolls towards newer messages as contentY FALLS.
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
                // A wheel glide still in flight would fight the gesture, so
                // it is cancelled once when the gesture starts rather than on
                // every tick. stickToBottom is NOT forced false here:
                // updateStickAndPaginate() derives it from the real position,
                // so autoscrolling back down to the newest message re-arms
                // follow-latest exactly as the wheel does.
                onActiveChanged: if (active) {
                    timeline.cancelWheelMotion()
                    // Same reasoning as the wheel handler: starting an
                    // autoscroll gesture is the reader taking the view.
                    timeline.noteReaderTookControl()
                }
                onScrolled: {
                    timeline.updateStickAndPaginate()
                    // The scroller writes contentY directly, which leaves
                    // Flickable.moving false, and updateStickAndPaginate()
                    // does not touch the settle timer — so for the whole
                    // gesture userScrollActive was FALSE. That is not a
                    // cosmetic detail: captureViewAnchor() never ran, so
                    // every coalesced content-height change (a pagination
                    // prepend, an image or link preview resolving, a late
                    // decryption) reached maintainViewAnchor()'s IDLE branch
                    // and ABSOLUTELY restored contentY to the pre-gesture
                    // anchor — a teleport back to where the reader started.
                    // saveRoomPosition() never ran either.
                    //
                    // Restarting the settle timer makes the autoscroll a
                    // first-class scroll owner: the same re-base branch, the
                    // same settle-time anchor capture, position save and row
                    // window pass the wheel already gets. Deliberately NOT a
                    // fourth term on userScrollActive — one settle path is
                    // what the wheel relies on.
                    scrollSettleTimer.restart()
                }
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

            // Empty room — a real start-of-conversation block, not one line
            // of grey text floating in a void. Element gives this state an
            // identity (the room's own avatar and name), one honest sentence
            // and the actions that belong there; three of Lightning's four
            // panes gave it a single muted Label, and this is the one a new
            // user lands on first.
            //
            // An UNROTATED sibling of the timeline: the label this replaces
            // was a child of the rotation: 180 Flickable and therefore drew
            // upside down.
            //
            // Wording stays deliberately modest. `count === 0 &&
            // presentationReady` means "nothing is loaded", which is not the
            // same claim as "this room has no history" — a room whose
            // backfill has not produced a visible row yet reaches this state
            // too — so the headline says what is known and the supporting
            // line invites the obvious next action instead of asserting the
            // beginning of the conversation.
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
                    // Stated only when the SDK says the room is encrypted —
                    // never as reassuring decoration on a room whose state we
                    // have not read.
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: AppTheme.spacingXS
                        visible: root.currentRoom.encrypted === true
                        Icon {
                            // Neutral, not green: green would read as a
                            // verification badge, and this is a room-state
                            // fact, not a trust claim.
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
                            // Fails CLOSED: the roster controller may be
                            // pointed at another room (a Space home, the
                            // info panel's room), and an offer we cannot
                            // honour is worse than no offer.
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
                // Hidden the moment the trip home starts, not only once it
                // lands: stickToBottom now flips at ARRIVAL on the glide
                // path, and the pill lingering through the glide would be a
                // regression from the old instant hide (review find).
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

        // Typing indicator — a constant-height slot while a room is shown.
        // It used to be a conditional row, so every appearance/disappearance
        // resized the timeline viewport and the re-pin handler shifted the
        // whole message stack by the indicator's height (the reported
        // "jitters the chat up and down"). Only the label's opacity changes
        // now; the reserved strip never moves the timeline.
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
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                opacity: text.length > 0 ? 1 : 0
                // The slot is always present; keep the EMPTY state out of
                // the accessibility tree.
                Accessible.ignored: text.length === 0
                elide: Text.ElideRight
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.italic: true
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }

        // v0.5.9: Save As result feedback (auto-clears).
        //
        // v0.6.7: it reserves NO layout height. It used to be an ordinary
        // row, so every appearance pushed the whole timeline up and every
        // auto-clear dropped it back down — a visible jump on something as
        // routine as saving a GIF. The wrapper is a zero-height layout item
        // and the banner is anchored to its bottom, overflowing upward over
        // the timeline instead of displacing it.
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

        // The composer has no target when no room is selected — the Home
        // surface is shown instead. visible:false collapses its space.
        MessageComposerBar {
            id: messageComposer
            objectName: "messageComposer"
            Layout.fillWidth: true
            // Visible DURING a call too. It was hidden because the stage was
            // a full-column surface and a composer beneath a timeline nobody
            // could see was a second bottom bar for nothing. The timeline is
            // on screen now, so being able to type in it is the point.
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
    // The 1px rule is also the resize grab: a 5px transparent band carrying
    // the line, exactly like the shell SplitView's own handle. It used to be
    // a bare 1px Rectangle, so the panel a tester specifically named — "the
    // member list panel" — was the one panel in the window that could not be
    // resized. The width persists (SettingsManager::sidePanelWidth), and the
    // clamp lives in the setter rather than here.
    Item {
        id: infoResizer
        objectName: "roomInfoResizeHandle"
        visible: root.infoOpen
        Layout.fillHeight: true
        implicitWidth: 5

        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: parent.height
            color: infoDrag.active ? AppTheme.accent
                 : infoHover.hovered ? AppTheme.borderStrong : AppTheme.border
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        HoverHandler {
            id: infoHover
            cursorShape: Qt.SplitHCursor
        }
        DragHandler {
            id: infoDrag
            target: null
            yAxis.enabled: false
            // The width AT GRAB, because DragHandler.translation is measured
            // from the press and is cumulative — applying it as a delta on
            // every change would multiply the movement.
            property int startWidth: 0
            onActiveChanged: {
                if (active)
                    startWidth = app.settings.sidePanelWidth
            }
            onTranslationChanged: {
                if (!active)
                    return
                // The panel is on the RIGHT, so dragging left widens it.
                app.settings.sidePanelWidth =
                    Math.round(startWidth - translation.x)
            }
        }
    }
    RoomInfoPanel {
        id: infoPanel
        objectName: "roomInfoPanel"
        Layout.fillHeight: true
        // CAPPED AGAINST WHAT IS ACTUALLY THERE, not just the stored width.
        //
        // The stored width is whatever the user last dragged it to, and it
        // outlives the window it was dragged in. When the window is later
        // narrower than (conversation minimum + this), the row's total
        // minimum exceeds its width and QtQuickLayouts overflows to the
        // RIGHT — so the panel ran off the window edge with its last tab, its
        // Save buttons and half its wrapped text outside the frame.
        //
        // 420 is the conversation's floor: below that the timeline stops
        // being a conversation. The panel's own floor is 260, and the
        // `>= 700` gate below means the pane is never narrow enough for the
        // two to fight.
        Layout.preferredWidth: root.infoOpen
            ? Math.max(260, Math.min(app.settings.sidePanelWidth,
                                     root.width - 420))
            : 0
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
        // v0.7.x pinned messages: a pin is very often outside the loaded
        // window, which is exactly what PaginationController::jumpToEvent
        // already handles for replies, permalinks and search hits. Reusing
        // it means pins hydrate through the ONE navigation path instead of
        // a second one that would have to re-learn anchoring.
        onJumpToEventRequested: (eventId) => {
            if (eventId !== "")
                app.pagination.jumpToEvent(eventId)
        }
    }

    // ── Room message search side panel ──────────────────────────────────
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

    // v0.7: Space Home — the management surface for a selected Space
    // (never an ordinary timeline). Hierarchy data is authoritative
    // SpaceManager state (m.space.child via sync); "Add existing room"
    // sends the real state event and the list updates when sync confirms.
    Component {
        id: spaceHomeComponent
        Rectangle {
            id: spaceHome
            objectName: "spaceHomePane"
            // The PANE ground, not `surface`. Space Home fills the content
            // area exactly as the timeline does, and `surface` is the raised
            // card tone — so it rendered as one enormous card and read as the
            // palest thing on screen ("even more pale", 2026-08-21). Cards
            // INSIDE it still use surface/cardElevated, which is what gives
            // them their lift.
            color: AppTheme.background

            readonly property string spaceId:
                app.spaces ? app.spaces.activeSpaceId : ""
            property var info: ({})
            property var childRooms: []
            // 2026-08-18 ("Land of the Insane"): JOINED sub-spaces of this
            // space, shown nested above the room list; clicking one drills
            // into its own Space Home.
            property var childSpaces: []
            // v0.7.x: /hierarchy children the account has not joined
            // (join offers). Refreshed through RoomDiscoveryController.
            property var unjoinedChildren: []
            // Full /hierarchy snapshot (joined rows included) — the
            // unified list's source for suggested flags + member counts.
            property var hierarchyRows: []
            property string addNotice: ""
            property bool settingsOpen: false
            // A Space IS a Matrix room, so it has a real member list — the
            // same roster Room Information reads, already pointed at this
            // Space (canManageChildren below relies on exactly that). It was
            // simply never surfaced here.
            property bool peopleOpen: false

            InvitePeopleDialog {
                id: spaceInviteDialog
                parent: Overlay.overlay
            }

            Connections {
                target: app.discovery
                // Joining a sub-space from the offers below drills straight
                // into it: its rooms live nested under ITS Home (Element's
                // behavior for the same layout). The signal is GLOBAL —
                // every successful space join through the discovery
                // controller emits it (the Discover dialog included) — so
                // only drill when the joined space is one of THIS space's
                // own offers; an unrelated join must never yank the user
                // out of the Home they are looking at (review find,
                // 2026-08-18).
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
                childRooms = app.spaces
                           ? app.spaces.childRoomsDetailed(spaceId) : []
                childSpaces = app.spaces
                            ? app.spaces.childSpacesDetailed(spaceId) : []
                refreshUnjoined()
            }
            function refreshUnjoined() {
                if (spaceId === "" || !app.discovery.supported) {
                    hierarchyRows = []
                    unjoinedChildren = []
                    return
                }
                var rows = app.discovery.spaceChildren(spaceId)
                // The full /hierarchy snapshot annotates JOINED rows too
                // (suggested flag, member counts) — the unified list
                // reads both slices of it.
                hierarchyRows = rows
                var out = []
                for (var i = 0; i < rows.length; ++i) {
                    if (rows[i].membership !== "joined")
                        out.push(rows[i])
                }
                unjoinedChildren = out
            }
            // ELEMENT-PARITY unified child list (2026-08-19): joined
            // subspaces, joined rooms and unjoined /hierarchy offers in
            // ONE "Rooms and spaces" list — each row states its own
            // membership ("Joined" badge) instead of three separate
            // headers. suggested/members ride the hierarchy rows and are
            // shown only when KNOWN, never fabricated.
            function buildUnifiedRows(subspaces, rooms, offers, hrows,
                                      filter) {
                var meta = {}
                var i
                for (i = 0; i < hrows.length; ++i)
                    meta[hrows[i].roomId] = hrows[i]
                var f = (filter || "").toLowerCase()
                function matches(name, topic) {
                    if (f === "")
                        return true
                    return (name || "").toLowerCase().indexOf(f) >= 0
                           || (topic || "").toLowerCase().indexOf(f) >= 0
                }
                var out = []
                var seen = {}
                for (i = 0; i < subspaces.length; ++i) {
                    var cs = subspaces[i]
                    var csm = meta[cs.roomId] || {}
                    seen[cs.roomId] = true
                    if (!matches(cs.name, csm.topic))
                        continue
                    out.push({
                        roomId: cs.roomId, name: cs.name || "",
                        avatarUrl: cs.avatarUrl || "",
                        identityColorKey: cs.identityColorKey || "",
                        isSpace: true, joined: true, isDirect: false,
                        suggested: csm.suggested === true,
                        suggestedKnown: csm.suggested !== undefined,
                        members: Number(csm.members || 0),
                        childCount: Number(cs.childCount || 0),
                        childrenCount: 0, hasUnread: false,
                        unreadCount: 0, highlightCount: 0,
                        membership: "joined", joinRule: "", via: []
                    })
                }
                for (i = 0; i < rooms.length; ++i) {
                    var cr = rooms[i]
                    var crm = meta[cr.roomId] || {}
                    seen[cr.roomId] = true
                    if (!matches(cr.name, crm.topic))
                        continue
                    out.push({
                        roomId: cr.roomId, name: cr.name || "",
                        avatarUrl: cr.avatarUrl || "",
                        identityColorKey: cr.identityColorKey || "",
                        isSpace: false, joined: true,
                        isDirect: cr.isDirect === true,
                        suggested: crm.suggested === true,
                        suggestedKnown: crm.suggested !== undefined,
                        members: Number(crm.members || 0),
                        childCount: 0, childrenCount: 0,
                        hasUnread: cr.hasUnread === true,
                        unreadCount: Number(cr.unreadCount || 0),
                        highlightCount: Number(cr.highlightCount || 0),
                        membership: "joined", joinRule: "", via: []
                    })
                }
                for (i = 0; i < offers.length; ++i) {
                    var uo = offers[i]
                    // Dedup by room id (review find): right after a Join
                    // succeeds, sync marks the room joined FAST while the
                    // /hierarchy refetch is still in flight — without
                    // this, the same room renders both "Joined" and as a
                    // stale Join offer for one network round trip. The
                    // joined arrays are authoritative sync state and win.
                    if (seen[uo.roomId] === true)
                        continue
                    if (!matches(uo.name, uo.topic))
                        continue
                    out.push({
                        roomId: uo.roomId, name: uo.name || "",
                        avatarUrl: uo.avatarUrl || "",
                        identityColorKey: "",
                        isSpace: uo.isSpace === true, joined: false,
                        isDirect: false,
                        suggested: uo.suggested === true,
                        suggestedKnown: uo.suggested !== undefined,
                        members: Number(uo.members || 0),
                        childCount: 0,
                        childrenCount: Number(uo.childrenCount || 0),
                        hasUnread: false, unreadCount: 0,
                        highlightCount: 0,
                        membership: uo.membership || "",
                        joinRule: uo.joinRule || "", via: uo.via || []
                    })
                }
                return out
            }
            property string childFilter: ""
            property var selectedChildIds: ({})
            readonly property int selectedCount:
                Object.keys(selectedChildIds).length
            readonly property var unifiedRows:
                buildUnifiedRows(childSpaces, childRooms, unjoinedChildren,
                                 hierarchyRows, childFilter)
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
            // The suggest toggle mirrors Element: one button whose action
            // follows the selection — all-suggested flips off, otherwise on.
            function selectedAllSuggested() {
                var rows = unifiedRows
                var any = false
                for (var i = 0; i < rows.length; ++i) {
                    if (selectedChildIds[rows[i].roomId] !== true)
                        continue
                    any = true
                    if (rows[i].suggested !== true)
                        return false
                }
                return any
            }
            onSpaceIdChanged: {
                addNotice = ""
                selectedChildIds = ({})
                childFilter = ""
                removeChildConfirm.close()
                removeChildConfirm.roomIds = []
                refresh()
                if (spaceId !== "" && app.discovery.supported)
                    app.discovery.refreshSpaceChildren(spaceId)
                // Point RoomInfoController at the space while its Home is
                // on screen: the Invite button's canInvite gate and the
                // settings card both read it (a Space never becomes
                // app.currentRoomId, so nothing contends here).
                if (spaceId !== "" && app.roomInfo)
                    app.roomInfo.roomId = spaceId
            }
            Component.onCompleted: {
                refresh()
                if (spaceId !== "" && app.discovery.supported)
                    app.discovery.refreshSpaceChildren(spaceId)
                if (spaceId !== "" && app.roomInfo)
                    app.roomInfo.roomId = spaceId
            }
            Connections {
                target: app.discovery
                function onSpaceChildrenChanged(changedSpaceId) {
                    if (changedSpaceId === spaceHome.spaceId)
                        spaceHome.refreshUnjoined()
                }
                // A join changes a row's membership; the hierarchy answer
                // is re-read so the offer disappears (the joined list
                // itself updates through authoritative sync).
                function onRoomJoined() {
                    if (spaceHome.spaceId !== "")
                        app.discovery.refreshSpaceChildren(spaceHome.spaceId)
                }
                function onKnockSent() {
                    if (spaceHome.spaceId !== "")
                        app.discovery.refreshSpaceChildren(spaceHome.spaceId)
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
                function onChildSuggestedFinished(spaceId, roomId,
                                                  suggested, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    if (!ok)
                        spaceHome.addNotice =
                            qsTr("The suggested flag could not be changed "
                                 + "— you may not have permission.")
                    // The flag lives on the /hierarchy rows — refetch so
                    // the badges follow the server's answer, success and
                    // rejection alike (never applied optimistically).
                    // COALESCED (review find): refreshSpaceChildren is
                    // single-flight with no queue, so a multi-select
                    // toggle firing N refetches would drop all but the
                    // first — one refetch after the burst instead.
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

                // Space banner — a REAL image, set by whoever the Space's
                // own power levels allow, under the event type Sable already
                // writes (see rust/src/banner.rs). A client that does not
                // know it renders no banner, and nothing about the Space is
                // damaged by that.
                //
                // Shown only when there IS one, or when this account could
                // add one — an empty strip on a Space nobody can change is
                // 190px of nothing.
                Rectangle {
                    id: spaceBannerCard
                    objectName: "spaceHomeBanner"
                    // FULL-BLEED, and the height comes from THE IMAGE.
                    //
                    // Sable renders a space banner edge to edge with
                    // object-fit: cover at 190px (resizable 56-500), and
                    // copying that alone did NOT stop the cropping — because
                    // `cover` crops whenever the box and the picture disagree
                    // about shape, and a fixed height on a pane whose width
                    // follows the window means they nearly always do.
                    //
                    // So the box takes the picture's own aspect ratio: at full
                    // width, a height of width/aspect shows ALL of it, with
                    // no crop and nothing letterboxed. Bounded top and bottom
                    // so one very tall or very wide image can neither take
                    // over the page nor disappear; between those bounds the
                    // whole banner is visible, which is what "it does not
                    // fit" was actually asking for.
                    //
                    // It sits OUTSIDE the centred 880px column deliberately —
                    // that column's width would change the shape again.
                    readonly property real bannerAspect:
                        (spaceBannerImage.status === Image.Ready
                         && spaceBannerImage.implicitHeight > 0)
                        ? (spaceBannerImage.implicitWidth
                           / spaceBannerImage.implicitHeight)
                        : 0
                    // CROPPED by default — a fixed strip, as Sable renders
                    // it, so every Space is the same shape and the rooms
                    // below stay where the eye expects them. Expanded takes
                    // the picture's own aspect ratio instead, which is what
                    // shows all of it. App-wide and remembered.
                    readonly property bool expanded:
                        app.settings.spaceBannerExpanded
                    x: 0
                    width: spaceScroll.width
                    height: Math.round(expanded && bannerAspect > 0
                        ? Math.max(120, Math.min(420, width / bannerAspect))
                        : 190)
                    clip: true
                    // Hidden entirely when the user has turned banners off,
                    // and never shown as an empty strip on a Space nobody can
                    // change.
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
                        // Same revision dependency as bannerMxc: the
                        // answer only becomes known when the room
                        // replies, and until then this is false, so the
                        // control is never offered on a guess.
                        var _dep = app.banners.revision
                        return app.banners.canSetRoomBanner(
                            spaceHome.spaceId)
                    }

                    // Asked once per Space per session; the write path
                    // re-asks itself, so a fresh banner appears without
                    // one.
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

                    // The empty state. Theme tones only — no avatar, no
                    // invented artwork.
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
                        // Expanded, the box above is already the picture's
                        // own shape, so Fit has nothing to letterbox and all
                        // of it is visible. Cropped, the strip is a fixed
                        // height and Crop fills it, as every other client
                        // renders a banner.
                        fillMode: spaceBannerCard.expanded
                                  ? Image.PreserveAspectFit
                                  : Image.PreserveAspectCrop
                        // Bounded decode. A banner is a big picture and the
                        // provider decodes what it is asked for; the aspect
                        // ratio the height above reads is preserved by
                        // scaling, so bounding this cannot change the shape.
                        sourceSize.width: 1600
                        asynchronous: true
                        visible: status === Image.Ready
                        readonly property string mxc:
                            spaceBannerCard.bannerMxc
                        // A counter, never an assignment to `source`:
                        // assigning a bound property imperatively DESTROYS
                        // the binding, which is what made Space banners
                        // sticky — the first one that finished loading
                        // unbound this Image from `mxc`, so every other
                        // Space kept showing it. See MemberProfilePopover
                        // for the same fix on the profile half.
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
                    // A wash under the controls, so they keep their
                    // contrast whatever the image happens to be.
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
                        // The picker CHOOSES; the crop dialog decides what is
                        // uploaded, and refuses anything that is not one of
                        // the five raster formats before it is rendered.
                        onAccepted: spaceBannerCrop.openFor(selectedFile)
                    }
                    ImageCropDialog {
                        id: spaceBannerCrop
                        role: "banner"
                        // The URL crosses as-is; the manager converts
                        // it. Stripping "file://" here produced "/C:/..."
                        // on Windows.
                        onCropped: function (file) {
                            app.banners.setRoomBanner(
                                spaceHome.spaceId, file.toString())
                        }
                    }

                    // Every banner control in ONE cluster, on a scrim.
                    //
                    // They were text buttons with a transparent field sitting
                    // directly on the photograph: "Change banner" in outline
                    // blue over a bright blue planet, "Remove" in danger red
                    // over black space. A control drawn on an arbitrary
                    // picture has no background to have contrast WITH, so it
                    // gets one — a dark pill it can be legible on whatever is
                    // underneath, which is the same reason the wash below the
                    // image exists.
                    //
                    // Icons rather than words for the same reason: the corner
                    // of a photograph is not where a sentence belongs, and a
                    // tooltip says the rest. The scrim and its ink come from
                    // the media-chrome tokens the GIF and size pills already
                    // use — the same problem, already solved once.
                    Rectangle {
                        objectName: "spaceBannerControls"
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: AppTheme.spacing8
                        radius: height / 2
                        // The media-chrome tokens, not a literal: this is
                        // the same problem the GIF and size pills already
                        // solved — a control that has to read on top of an
                        // arbitrary picture.
                        color: AppTheme.scrimSurface
                        border.width: 1
                        border.color: AppTheme.overlayScrim
                        readonly property int pad: AppTheme.spacing4
                        implicitWidth: bannerControlRow.implicitWidth + pad * 2
                        implicitHeight: bannerControlRow.implicitHeight + pad * 2
                        // Visibility comes from the CONDITIONS, never from the
                        // children's `visible`.
                        //
                        // Summing the children latched this pill off for good.
                        // QQuickItem::visible is EFFECTIVE visibility: while
                        // the pill is hidden every child reports false however
                        // its own binding evaluates, so the sum stays zero,
                        // and a child turning itself on changes no effective
                        // value and therefore notifies nothing. At startup the
                        // banner has not been fetched yet, so the count starts
                        // at zero — and never left it. Reproduced in isolation
                        // before this was changed; the same shape as the
                        // busy-indicator latch already recorded in CLAUDE.md.
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
                                // Destructive, and legible on the scrim. NOT
                                // AppTheme.danger: that ink is tuned to be
                                // read on a theme surface, and this sits on a
                                // photograph. The washed danger tone the
                                // media chrome already uses reads on both.
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

                    // A refusal is reported where it happened, and
                    // nothing was applied optimistically to undo.
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
                    // Measured against the FLICKABLE, not against `parent`.
                    // A Flickable reparents its children to contentItem, and
                    // centring against that item put the whole column far to
                    // the right of a wide window with a large empty area
                    // beside it. The viewport is what the column should be
                    // centred in, so it is named outright.
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
                            size: 56
                            name: spaceHome.info.name || ""
                            mxc: spaceHome.info.avatarUrl || ""
                            colorKey: spaceHome.spaceId
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                objectName: "spaceHomeName"
                                text: spaceHome.info.name || qsTr("Space")
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
                            objectName: "spaceInviteButton"
                            // A Space IS a Matrix room: same invite path,
                            // same server-side permission gate, surfaced
                            // only when the roster says we may invite
                            // (2026-08-18 tester report #2, MEDIUM).
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
                            // Gated on the roster actually being THIS Space's:
                            // app.roomInfo follows the Room Information panel,
                            // which may still be pointing at a room.
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
                        // The way back. Hiding the banner from its own corner
                        // would otherwise be one-way, and a control that can
                        // only be turned off is a trap. Offered only where
                        // there is actually a banner to bring back.
                        AppButton {
                            objectName: "spaceBannerShowButton"
                            visible: !app.settings.spaceBannersVisible
                                     && spaceBannerCard.bannerMxc.length > 0
                            iconName: "visibility"
                            text: qsTr("Show banner")
                            onClicked: app.settings.spaceBannersVisible = true
                        }
                        Item { Layout.fillWidth: true }
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

                    // Space members. The same roster Room Information reads,
                    // which is already pointed at this Space — a Space IS a
                    // Matrix room, so its members are real members and nothing
                    // Space-specific is invented to list them.
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
                                    // BOUNDED: a large Space would otherwise
                                    // instantiate one delegate per member in a
                                    // non-virtualized Flow, on the frame the
                                    // card becomes visible.
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
                                // The cap is disclosed rather than silently
                                // truncating: a Space with 200 members must
                                // not look like it has 60.
                                text: qsTr("Showing the first 60 of %1.")
                                      .arg((app.roomInfo.members || []).length)
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
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
                        // roomInfo now binds to the space for the whole
                        // Space Home lifetime (see Component.onCompleted
                        // below) — the card no longer needs its own bind.
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
                            // Same permission-gated backend as a room's own
                            // avatar — a Space IS a Matrix room, so this is
                            // m.room.avatar either way and Lightning invents
                            // no Space-specific storage.
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
                                    // canEditAvatar is the room's REAL
                                    // required level for m.room.avatar, read
                                    // from the member snapshot — never a role
                                    // label and never optimistic.
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

                    // ELEMENT-PARITY (2026-08-19): ONE "Rooms and
                    // spaces" list — joined subspaces, joined rooms and
                    // unjoined /hierarchy offers together, each row
                    // carrying its own "Joined" badge, with Element's
                    // selection UI (checkboxes + Remove + the suggested
                    // toggle) gated on the REAL m.space.child send level.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacingS
                        spacing: AppTheme.spacing8
                        Label {
                            // NOTE: the uppercase string is pinned by
                            // SpaceSettingsContractTest (which uses it as a
                            // section end marker) and ElementParityContract-
                            // Test, so the sentence-case section recipe the
                            // design system introduced cannot land here
                            // without moving those two files in the same
                            // change. The weight and size are on the scale;
                            // only the casing is still legacy.
                            text: qsTr("ROOMS AND SPACES")
                            color: AppTheme.textSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.weight: AppTheme.weightStrong
                            font.letterSpacing: 0.8
                        }
                        Label {
                            visible: spaceHome.selectedCount > 0
                            text: qsTr("%n selected", "",
                                       spaceHome.selectedCount)
                            color: AppTheme.chipAccentInk
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.weight: AppTheme.weightMedium
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            objectName: "spaceChildRemoveSelectedButton"
                            visible: spaceHome.canManageChildren
                            kind: "danger"
                            enabled: spaceHome.selectedCount > 0
                            text: qsTr("Remove")
                            onClicked: {
                                removeChildConfirm.roomIds =
                                    Object.keys(spaceHome.selectedChildIds)
                                removeChildConfirm.open()
                            }
                        }
                        AppButton {
                            objectName: "spaceChildSuggestToggleButton"
                            visible: spaceHome.canManageChildren
                            enabled: spaceHome.selectedCount > 0
                            text: spaceHome.selectedAllSuggested()
                                  ? qsTr("Mark as not suggested")
                                  : qsTr("Mark as suggested")
                            onClicked: {
                                var want = !spaceHome.selectedAllSuggested()
                                var ids = Object.keys(
                                    spaceHome.selectedChildIds)
                                for (var i = 0; i < ids.length; ++i)
                                    app.spaces.setSpaceChildSuggested(
                                        spaceHome.spaceId, ids[i], want)
                                spaceHome.selectedChildIds = ({})
                            }
                        }
                    }
                    AppTextField {
                        objectName: "spaceChildFilterField"
                        Layout.fillWidth: true
                        searchIcon: true
                        clearButton: true
                        placeholderText:
                            qsTr("Search names and descriptions")
                        Accessible.name: qsTr("Search rooms and spaces")
                        text: spaceHome.childFilter
                        onTextChanged: spaceHome.childFilter = text
                    }

                    // A filter with no matches says so — a silently
                    // blank list reads as stuck (2026-08-19 audit).
                    Label {
                        visible: spaceHome.unifiedRows.length === 0
                                 && spaceHome.childFilter !== ""
                        Layout.fillWidth: true
                        text: qsTr("No rooms or spaces match “%1”.")
                                  .arg(spaceHome.childFilter)
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.Wrap
                    }

                    // Empty state for a fresh Space.
                    Rectangle {
                        visible: spaceHome.unifiedRows.length === 0
                                 && spaceHome.childFilter === ""
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
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                                font.weight: AppTheme.weightStrong
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Create a room here or add one of "
                                           + "your existing rooms to organise "
                                           + "it under this Space.")
                                color: AppTheme.textSecondary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                wrapMode: Text.WordWrap
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                            }
                        }
                    }

                    Repeater {
                        model: spaceHome.unifiedRows
                        delegate: Rectangle {
                            id: unifiedRow
                            required property var modelData
                            objectName: "spaceUnifiedChildRow"
                            readonly property bool rowSelected:
                                spaceHome.selectedChildIds[
                                    modelData.roomId] === true
                            readonly property bool rowKnocks:
                                modelData.joinRule === "knock"
                                || modelData.joinRule === "knock_restricted"
                            Layout.fillWidth: true
                            implicitHeight: 50
                            radius: AppTheme.radiusMd
                            // Hover feedback only where a click acts —
                            // offers act through their Join button alone
                            // (the pin-row rule in RoomInfoPanel).
                            color: unifiedHover.hovered
                                   && unifiedRow.modelData.joined === true
                                   ? AppTheme.hover : "transparent"
                            HoverHandler { id: unifiedHover }
                            TapHandler {
                                // Joined rows open; a joined sub-space
                                // drills into its own Home (its rooms are
                                // nested there — never a join). Offers act
                                // only through their Join button. The
                                // selection checkbox's band is excluded:
                                // TapHandlers are non-exclusive across
                                // subtrees, so without the guard a select
                                // tap would ALSO open the row.
                                onTapped: (eventPoint) => {
                                    if (selectBox.visible) {
                                        var sp = unifiedRow.mapToItem(
                                            selectBox,
                                            eventPoint.position.x,
                                            eventPoint.position.y)
                                        if (sp.x >= 0
                                            && sp.x <= selectBox.width
                                            && sp.y >= 0
                                            && sp.y <= selectBox.height)
                                            return
                                    }
                                    if (unifiedRow.modelData.joined !== true)
                                        return
                                    if (unifiedRow.modelData.isSpace)
                                        app.spaces.activeSpaceId =
                                            unifiedRow.modelData.roomId
                                    else
                                        app.openRoom(
                                            unifiedRow.modelData.roomId)
                                }
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: unifiedRow.modelData.name
                                             || unifiedRow.modelData.roomId
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacingS
                                anchors.rightMargin: AppTheme.spacingS
                                spacing: AppTheme.spacingS
                                Avatar {
                                    size: 32
                                    circle: unifiedRow.modelData.isDirect
                                            === true
                                    name: unifiedRow.modelData.name || ""
                                    mxc: unifiedRow.modelData.avatarUrl || ""
                                    colorKey: unifiedRow.modelData
                                                  .identityColorKey
                                              || unifiedRow.modelData.roomId
                                              || ""
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing6
                                        Label {
                                            text: unifiedRow.modelData.name
                                                  || qsTr("Room")
                                            color: AppTheme.text
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(
                                                AppTheme.textBody)
                                            font.weight:
                                                unifiedRow.modelData.hasUnread
                                                ? AppTheme.weightBold
                                                : AppTheme.weightMedium
                                            elide: Label.ElideRight
                                            // Measured against the ROW, never
                                            // against `parent` (this
                                            // RowLayout): a cap read from the
                                            // layout's own arranged width is
                                            // an input the layout produces, so
                                            // Qt Quick Layouts logged
                                            // "Detected recursive rearrange"
                                            // once per pass for every row in
                                            // a Space's list. unifiedRow is
                                            // fillWidth in the outer column
                                            // and reports no implicit width of
                                            // its own, so it cannot feed back.
                                            Layout.maximumWidth:
                                                unifiedRow.width * 0.7
                                        }
                                        // Element parity: the row itself
                                        // says whether the account is in
                                        // it — one list, honest badges.
                                        // Joined and Suggested are two
                                        // different KINDS of fact, so they
                                        // take two different chip families
                                        // rather than a loose green ink and
                                        // a grey outline: membership is a
                                        // success state, "suggested" is the
                                        // Space owner's recommendation.
                                        Rectangle {
                                            visible: unifiedRow.modelData
                                                         .joined === true
                                            implicitHeight: AppTheme.chipHeight
                                            implicitWidth: joinedChipRow.implicitWidth
                                                           + AppTheme.chipPaddingH * 2
                                            radius: AppTheme.chipRadius
                                            color: AppTheme.chipSuccessFill
                                            border.color: AppTheme.chipSuccessBorder
                                            border.width: 1
                                            Row {
                                                id: joinedChipRow
                                                anchors.centerIn: parent
                                                spacing: 2
                                                Icon {
                                                    anchors.verticalCenter:
                                                        parent.verticalCenter
                                                    name: "check"
                                                    size: 12
                                                    color: AppTheme.chipSuccessInk
                                                }
                                                Label {
                                                    anchors.verticalCenter:
                                                        parent.verticalCenter
                                                    text: qsTr("Joined")
                                                    color: AppTheme.chipSuccessInk
                                                    font.family: AppTheme.uiFont
                                                    font.pixelSize: AppTheme.scaled(
                                                        AppTheme.textMicro)
                                                    font.weight: AppTheme.weightStrong
                                                }
                                            }
                                        }
                                        Label {
                                            visible: unifiedRow.modelData
                                                         .suggested === true
                                            text: qsTr("Suggested")
                                            color: AppTheme.chipAccentInk
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(
                                                AppTheme.textMicro)
                                            font.weight: AppTheme.weightStrong
                                            leftPadding: AppTheme.chipPaddingH
                                            rightPadding: AppTheme.chipPaddingH
                                            topPadding:
                                                AppTheme.keycapPaddingV
                                            bottomPadding:
                                                AppTheme.keycapPaddingV
                                            background: Rectangle {
                                                radius: AppTheme.chipRadius
                                                color: AppTheme.chipAccentFill
                                                border.color: AppTheme.chipAccentBorder
                                                border.width: 1
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        text: {
                                            var d = unifiedRow.modelData
                                            if (d.isSpace && d.joined)
                                                return qsTr(
                                                    "Space · %n room(s)", "",
                                                    Number(d.childCount || 0))
                                            if (d.isSpace)
                                                return qsTr(
                                                    "Space · %n room(s) inside",
                                                    "",
                                                    Number(d.childrenCount
                                                           || 0))
                                            if (d.members > 0)
                                                return qsTr("%n member(s)",
                                                            "",
                                                            Number(d.members))
                                            return ""
                                        }
                                        color: AppTheme.textMuted
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(
                                            AppTheme.textMeta)
                                        elide: Label.ElideRight
                                    }
                                }
                                Rectangle {
                                    visible: (unifiedRow.modelData
                                                  .highlightCount || 0) > 0
                                    radius: height / 2
                                    color: AppTheme.dangerFill
                                    implicitHeight: 18
                                    implicitWidth: Math.max(
                                        18, childMention.implicitWidth + 10)
                                    Label {
                                        id: childMention
                                        anchors.centerIn: parent
                                        text: "@"
                                        color: AppTheme.dangerText
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(
                                            AppTheme.textMicro)
                                        font.weight: AppTheme.weightBold
                                    }
                                }
                                Rectangle {
                                    visible: unifiedRow.modelData.hasUnread
                                             === true
                                    radius: height / 2
                                    color: AppTheme.unreadBadge
                                    implicitHeight: 18
                                    implicitWidth: Math.max(
                                        18, childCount.implicitWidth + 10)
                                    Label {
                                        id: childCount
                                        anchors.centerIn: parent
                                        visible: (unifiedRow.modelData
                                                      .unreadCount || 0) > 0
                                        text: unifiedRow.modelData.unreadCount
                                              > 99
                                              ? "99+"
                                              : unifiedRow.modelData
                                                    .unreadCount
                                        color: AppTheme.boltInk
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(
                                            AppTheme.textMicro)
                                        font.weight: AppTheme.weightBold
                                    }
                                }
                                Label {
                                    visible: unifiedRow.modelData.membership
                                             === "knocked"
                                    text: qsTr("Request pending")
                                    color: AppTheme.chipWarningInk
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.scaled(
                                        AppTheme.textMicro)
                                    font.weight: AppTheme.weightStrong
                                    leftPadding: AppTheme.chipPaddingH
                                    rightPadding: AppTheme.chipPaddingH
                                    topPadding: AppTheme.keycapPaddingV
                                    bottomPadding: AppTheme.keycapPaddingV
                                    background: Rectangle {
                                        radius: AppTheme.chipRadius
                                        color: AppTheme.chipWarningFill
                                        border.color: AppTheme.chipWarningBorder
                                        border.width: 1
                                    }
                                }
                                AppButton {
                                    visible: unifiedRow.modelData.joined
                                             !== true
                                             && unifiedRow.modelData
                                                    .membership !== "knocked"
                                    kind: "primary"
                                    enabled: !app.discovery.busy
                                    text: unifiedRow.rowKnocks
                                          ? qsTr("Ask to join") : qsTr("Join")
                                    onClicked: {
                                        var via = unifiedRow.modelData.via
                                                  || []
                                        if (unifiedRow.rowKnocks)
                                            app.discovery.knock(
                                                unifiedRow.modelData.roomId,
                                                via, "")
                                        else
                                            app.discovery.join(
                                                unifiedRow.modelData.roomId,
                                                via,
                                                unifiedRow.modelData.isSpace
                                                === true)
                                    }
                                }
                                Icon {
                                    visible: unifiedRow.modelData.isSpace
                                             === true
                                             && unifiedRow.modelData.joined
                                                === true
                                    name: "chevron_right"
                                    size: 16
                                    color: AppTheme.textMuted
                                }
                                // Element's selection UI: a per-row
                                // checkbox shown only when the account can
                                // actually send m.space.child here. The
                                // row handler excludes this band — two
                                // TapHandlers in unrelated subtrees BOTH
                                // fire on one tap (this round's lesson).
                                Item {
                                    id: selectBox
                                    objectName: "spaceChildSelectBox"
                                    visible: spaceHome.canManageChildren
                                    Layout.preferredWidth: 26
                                    Layout.preferredHeight: 26
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18; height: 18
                                        radius: 4
                                        color: unifiedRow.rowSelected
                                               ? AppTheme.accent
                                               : "transparent"
                                        border.color: unifiedRow.rowSelected
                                                      ? AppTheme.accent
                                                      : AppTheme.borderStrong
                                        border.width: 1
                                        Icon {
                                            anchors.centerIn: parent
                                            visible: unifiedRow.rowSelected
                                            name: "check"
                                            size: 13
                                            color: AppTheme.accentText
                                        }
                                    }
                                    TapHandler {
                                        onTapped:
                                            spaceHome.toggleChildSelected(
                                                unifiedRow.modelData.roomId)
                                    }
                                    Accessible.role: Accessible.CheckBox
                                    Accessible.name:
                                        qsTr("Select %1").arg(
                                            unifiedRow.modelData.name || "")
                                }
                            }
                        }
                    }
                    Label {
                        visible: app.discovery.errorMessage.length > 0
                                 && spaceHome.unifiedRows.length > 0
                        Layout.fillWidth: true
                        text: app.discovery.errorMessage
                        color: AppTheme.danger
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.Wrap
                    }
                }
            }

            // Child-removal confirmation. Destructive only for the
            // hierarchy relation — never the room.
            Popup {
                id: removeChildConfirm
                // 2026-08-19: driven by the unified list's SELECTION —
                // one confirm for N rooms. Destructive only for the
                // hierarchy relation, never the rooms themselves.
                property var roomIds: []
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                // The app's floating-dialog dialect (2026-08-19 audit):
                // storm surface + the shared navy modal scrim, like every
                // other confirm in the app.
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

            // Leave confirmation — leaving a Space never touches its rooms.
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

    // Files dragged anywhere over the CHAT — not only over the composer —
    // queue as attachments in the composer's tray (live feedback). DropArea
    // only consumes drag events, so scrolling and clicks are untouched.
    DropArea {
        id: chatDropArea
        // review H1: never cover the thread surface — ThreadPanel carries
        // its own DropArea routing to app.thread.addAttachment, and a
        // pane-wide acceptor here would hijack those drops into the ROOM
        // composer (CLAUDE.md section 8: thread attachments must use the
        // thread send path). Side-by-side layouts stop 340px short of the
        // right edge; the full-width thread layout (<660) collapses this
        // area to zero and the thread's own DropArea owns every drop.
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
        // The hint mirrors the drop area's thread-excluding geometry.
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
