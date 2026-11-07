import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: shellRoot

    // Escape steps back: with no room open a Space returns to Home. An open
    // room stays open, as in Element and Discord (this closed it from GitHub
    // #27 until the 0.10.3 QA round, which reported it as a lost room).
    // A Keys handler, not a Shortcut: Escape is reserved in ShortcutRegistry
    // and a window Shortcut takes the key before the focused item, so the
    // composer (closing a completion, cancelling a reply or an edit) and every
    // other Escape owner would lose it. This handler sees Escape only when
    // nothing under the focus accepted it; dialogs, menus and the image viewer
    // are popups, not children of this item, so they never reach it while
    // they hold the focus, and one that lost it is asked first. The
    // find bar, info panel, search, thread and pinned layers keep their
    // TimelinePane Shortcut, which runs first while one of them is open.
    Keys.onEscapePressed: (event) => {
        const focused = Window.activeFocusItem
        if (focused && (focused instanceof TextInput || focused instanceof TextEdit)
                && !shellRoot.insideComposer(focused)) {
            event.accepted = false
            return
        }
        // An open popup owns Escape even without the focus: the tap that
        // opened the read-receipt list also reached the timeline's own tap
        // handler, which took the focus back, so one Escape closed the
        // popover and went on. Close the popup and stop.
        if (app.dismissPopupForEscape(shellRoot)) {
            event.accepted = true
            return
        }
        event.accepted = app.navigateBack()
        if (event.accepted)
            shellRoot.forceActiveFocus()
    }
    function _descendantOf(item, ancestor) {
        for (var it = item; it; it = it.parent) {
            if (it === ancestor) return true
        }
        return false
    }
    function _firstTabStop(region) {
        var it = region
        for (var hops = 0; hops < 400; ++hops) {
            it = it.nextItemInFocusChain(true)
            if (!it || it === region) return null
            if (shellRoot._descendantOf(it, region) && it.visible && it.enabled)
                return it
        }
        return null
    }
    function focusRegion(region) {
        if (!region || !region.visible) return false
        if (region === timelinePane && timelinePane.focusComposer())
            return true
        var target = shellRoot._firstTabStop(region)
        if (!target) return false
        target.forceActiveFocus(Qt.TabFocusReason)
        return true
    }
    // The unverified-session card is a region of the F6 cycle while it shows,
    // after the chat (MASTER-3 M3). It lives on the window overlay, outside
    // this item, so Main.qml hands it in; and F6 pressed inside it never
    // reaches this item's Keys, so the card asks through regionCycleRequested.
    property Item promptRegion: null
    function _promptRegion() {
        var card = shellRoot.promptRegion
        return card && card.shouldShow && card.visible ? card : null
    }
    Connections {
        target: shellRoot.promptRegion
        ignoreUnknownSignals: true
        function onRegionCycleRequested(backward) { shellRoot.cycleRegions(backward) }
    }
    function cycleRegions(backward) {
        var regions = [spacesRail, roomsPanel, timelinePane,
                       shellRoot._promptRegion()].filter(
            r => r && r.visible && r.width > 0)
        if (regions.length === 0) return false
        var focused = Window.activeFocusItem
        var current = -1
        for (var i = 0; i < regions.length; ++i) {
            if (shellRoot._descendantOf(focused, regions[i])) { current = i; break }
        }
        for (var step = 1; step <= regions.length; ++step) {
            var next = backward
                ? (current < 0 ? regions.length - step
                               : (current - step + regions.length * 2) % regions.length)
                : (current < 0 ? step - 1 : (current + step) % regions.length)
            if (shellRoot.focusRegion(regions[next])) return true
        }
        return false
    }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F6
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                        | Qt.MetaModifier)))
            event.accepted = shellRoot.cycleRegions(
                !!(event.modifiers & Qt.ShiftModifier))
    }
    function insideComposer(item) {
        for (var it = item; it; it = it.parent) {
            if (it.objectName === "messageComposer")
                return true
        }
        return false
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("shell.toggleRoomList")]
        }
        onActivated: app.settings.roomListVisible = !app.settings.roomListVisible
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("shell.toggleSpacesRail")]
        }
        onActivated:
            app.settings.spacesRailVisible = !app.settings.spacesRailVisible
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.openSettings")]
        }
        enabled: app.currentScreen !== 2
        onActivated: app.requestSettingsSearchFocus()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.shortcutsHelp")]
        }
        onActivated: app.showSettingsSection("shortcuts")
    }

    function toggleMuteAction() {
        if (app.groupCall.active)
            app.groupCall.toggleMicrophoneMuted()
        else if (app.calls.muteControlAvailable)
            app.calls.toggleMicrophoneMuted()
    }
    function toggleDeafenAction() {
        if (app.groupCall.active)
            app.groupCall.toggleDeafened()
        else if (app.calls.muteControlAvailable)
            app.calls.toggleDeafened()
    }
    function leaveCallAction() {
        if (app.groupCall.active)
            app.groupCall.leave()
        else if (app.calls.activeRoomId !== "")
            app.calls.hangup()
    }
    Connections {
        target: app.globalShortcuts
        function onActivated(actionId) {
            if (actionId === "call.toggleMute")
                shellRoot.toggleMuteAction()
            else if (actionId === "call.toggleDeafen")
                shellRoot.toggleDeafenAction()
            else if (actionId === "call.leave")
                shellRoot.leaveCallAction()
        }
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleMute")]
        }
        onActivated: shellRoot.toggleMuteAction()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleDeafen")]
        }
        onActivated: shellRoot.toggleDeafenAction()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleCamera")]
        }
        onActivated: {
            if (app.groupCall.active)
                app.groupCall.toggleCamera()
        }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.returnToCall")]
        }
        onActivated: {
            var callRoomId = app.groupCall.active ? app.groupCall.roomId
                                                  : app.calls.activeRoomId
            if (callRoomId === "")
                return
            if (app.currentScreen !== 1)
                app.showMain()
            app.openRoom(callRoomId)
        }
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.startCall")]
        }
        // canStartCall() has no NOTIFY; the revision stands in for it and is
        // read first, since nothing after a false clause is ever bound.
        enabled: app.callGateRevision >= 0
                 && app.loggedIn && app.currentRoomId !== ""
                 && app.canStartCall(app.currentRoomId)
                 && !app.groupCall.active
                 && (app.calls.state === CallController.Idle
                     || app.calls.state === CallController.Ended)
        onActivated: app.startCall(app.currentRoomId, false)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.leave")]
        }
        onActivated: shellRoot.leaveCallAction()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleScreenShare")]
        }
        onActivated: {
            if (!app.groupCall.active)
                return
            if (app.groupCall.screenSharing)
                app.groupCall.stopScreenShare()
            else
                app.groupCall.requestScreenShare()
        }
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.quickSwitcher")]
        }
        onActivated: quickSwitcher.open()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.commandMode")]
        }
        onActivated: quickSwitcher.openCommandMode()
    }
    QuickSwitcher {
        id: quickSwitcher
        onDiscoverRequested: (startMode) => roomsPanel.openDiscover(startMode)
        onGlobalSearchRequested: (query) => messageSearchDialog.openDialog(query)
        onSavedMessagesRequested: savedMessagesPanel.openPanel()
        onThreadsRequested: roomsPanel.openThreadsActivity()
        onReturnToRoomRequested: {
            app.thread.close()
            app.requestComposerFocus(app.currentRoomId)
        }
        onFocusFallbackRequested: {
            if (app.currentRoomId !== "")
                app.requestComposerFocus(app.currentRoomId)
            else
                roomsPanel.focusNavigation()
        }
    }
    SavedMessagesPanel {
        id: savedMessagesPanel
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.savedMessages")]
        }
        enabled: app.loggedIn && !!app.savedMessages
        onActivated: savedMessagesPanel.openPanel()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.threadsActivity")]
        }
        enabled: app.loggedIn && !!app.activity && !quickSwitcher.visible
        onActivated: roomsPanel.openThreadsActivity()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("view.toggleLayoutPreset")]
        }
        enabled: app.loggedIn && !quickSwitcher.visible
        onActivated: app.settings.layoutPreset = app.settings.layoutPreset === 1 ? 0 : 1
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("thread.togglePlacement")]
        }
        enabled: app.loggedIn && app.thread.active && !quickSwitcher.visible
        onActivated: app.settings.threadPlacement = app.settings.threadPlacement === 1 ? 0 : 1
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("thread.openRootInRoom")]
        }
        enabled: app.loggedIn && app.thread.active
                 && app.thread.rootEventId !== "" && !quickSwitcher.visible
        onActivated: app.pagination.jumpToEvent(app.thread.rootEventId)
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.focus")]
        }
        enabled: timelinePane.composerShortcutAvailable && !quickSwitcher.visible
        onActivated: timelinePane.focusComposerShortcut()
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("timeline.selectLatestMessage")]
        }
        enabled: timelinePane.selectionShortcutAvailable && !quickSwitcher.visible
        onActivated: timelinePane.focusTimelineShortcut()
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("timeline.resumeMessageSelection")]
        }
        enabled: timelinePane.selectionShortcutAvailable && !quickSwitcher.visible
        onActivated: timelinePane.resumeTimelineShortcut()
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.messageSearch")]
        }
        enabled: app.loggedIn && app.messageSearch.supported
        onActivated: messageSearchDialog.openDialog()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.newConversation")]
        }
        enabled: app.loggedIn
        onActivated: roomsPanel.startConversation("room")
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.markRead")]
        }
        enabled: app.loggedIn && app.currentRoomId !== ""
        onActivated: app.roomList.markRoomRead(app.currentRoomId)
    }

    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.newDirectMessage")]
        }
        enabled: app.loggedIn
        onActivated: roomsPanel.startConversation("dm")
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.activityCenter")]
        }
        enabled: app.loggedIn && !!app.activity
        onActivated: roomsPanel.openActivityCenter()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.historyBack")]
        }
        enabled: app.loggedIn && app.currentScreen !== 2
        onActivated: app.navigateRoomHistoryFromKeyboard(shellRoot, false)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.historyForward")]
        }
        enabled: app.loggedIn && app.currentScreen !== 2
        onActivated: app.navigateRoomHistoryFromKeyboard(shellRoot, true)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.markAllRead")]
        }
        enabled: app.loggedIn
        onActivated: app.roomList.markAllRoomsRead()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.markUnread")]
        }
        enabled: app.loggedIn && app.currentRoomId !== ""
        onActivated: app.roomList.markRoomUnread(app.currentRoomId)
    }
    MessageSearchDialog {
        id: messageSearchDialog
        parent: Overlay.overlay
    }

    ScreenSharePicker {
        id: screenSharePicker
        parent: Overlay.overlay
    }

    Connections {
        target: app
        function onMatrixLinkRequested(link) {
            roomsPanel.openDiscoverForLink(link)
        }
    }

    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        if (obj.contentItem) {
            var viaContent = findDemoDescendant(obj.contentItem, name)
            if (viaContent) return viaContent
        }
        var kids = obj.children || []
        for (var i = 0; i < kids.length; ++i) {
            var found = findDemoDescendant(kids[i], name)
            if (found) return found
        }
        var data = obj.data || []
        for (var j = 0; j < data.length; ++j) {
            var found2 = findDemoDescendant(data[j], name)
            if (found2) return found2
        }
        return null
    }

    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenQuickSwitcher(query) {
            if (query && query.length > 0 && query[0] === ">") {
                quickSwitcher.openCommandMode()
                var rest = query.slice(1)
                Qt.callLater(function() {
                    var field = findDemoDescendant(quickSwitcher, "quickSwitcherField")
                    if (field) field.text = rest
                })
            } else {
                quickSwitcher.open()
                if (query && query.length > 0) {
                    Qt.callLater(function() {
                        var field = findDemoDescendant(quickSwitcher, "quickSwitcherField")
                        if (field) field.text = query
                    })
                }
            }
        }
    }

    function _pushMentionStyle() {
        var accent = "" + AppTheme.selfMentionInk
        var soft = "" + AppTheme.accentSoft
        var code = "" + AppTheme.codeBlock
        var linkInk = "" + AppTheme.link
        if (app.timeline && app.timeline.setMentionStyle)
            app.timeline.setMentionStyle(accent, soft, code, linkInk,
                                         AppTheme.underlineLinks)
        if (app.thread && app.thread.model && app.thread.model.setMentionStyle)
            app.thread.model.setMentionStyle(accent, soft, code, linkInk,
                                             AppTheme.underlineLinks)
    }
    Component.onCompleted: {
        applyDirection()
        _pushMentionStyle()
        // The mouse's Back / Forward buttons (see the room-history Shortcuts):
        // routed in C++ by a filter on the window, which judges a press before
        // any popup can close itself, so a press that dismisses a menu only
        // dismisses it.
        app.installMouseNavigation(shellRoot)
    }
    Connections {
        target: AppTheme
        function onSelfMentionInkChanged() { _pushMentionStyle() }
        function onAccentSoftChanged() { _pushMentionStyle() }
        function onCodeBlockChanged() { _pushMentionStyle() }
        function onLinkChanged() { _pushMentionStyle() }
        function onUnderlineLinksChanged() { _pushMentionStyle() }
    }

    property bool splitMirrored: false
    function applyDirection() {
        if (splitMirrored === app.localization.rightToLeft)
            return
        for (var i = 1; i < splitView.count; ++i)
            splitView.moveItem(i, 0)
        splitMirrored = app.localization.rightToLeft
    }
    Connections {
        target: app.localization
        function onLanguageChanged() { shellRoot.applyDirection() }
    }

    SplitView {
        id: splitView
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Item {
            id: splitHandle
            objectName: roomHandle ? "roomListDivider" : ""
            implicitWidth: 1
            readonly property bool roomHandle:
                Math.abs(x - (shellRoot.splitMirrored
                              ? roomsPanel.x - implicitWidth
                              : roomsPanel.x + roomsPanel.width)) < 3
            activeFocusOnTab: roomHandle && roomsPanel.visible
            Accessible.role: Accessible.Separator
            Accessible.name: qsTr("Resize room list")
            Keys.onPressed: (event) => {
                if (!roomHandle) return
                if (event.key === Qt.Key_Home) {
                    roomsPanel.collapse(); event.accepted = true
                } else if (event.key === Qt.Key_End) {
                    roomsPanel.expand(); event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (roomsPanel.compact) roomsPanel.expand()
                    else roomsPanel.collapse()
                    event.accepted = true
                } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                    const grow = shellRoot.splitMirrored ? Qt.Key_Left : Qt.Key_Right
                    if (roomsPanel.compact) {
                        if (event.key === grow) roomsPanel.expand()
                    } else {
                        const next = app.settings.roomListWidth
                                     + (event.key === grow ? 10 : -10)
                        if (next < app.settings.roomListExpandedMinWidth)
                            roomsPanel.collapse()
                        else {
                            app.settings.roomListWidth = Math.min(560, next)
                            app.settings.setRoomListExpandedWidth(app.settings.roomListWidth)
                        }
                    }
                    event.accepted = true
                }
            }
            property real pressWidth: -1
            property double lastClickMs: 0
            SplitHandle.onPressedChanged: {
                if (!splitHandle.roomHandle)
                    return
                if (SplitHandle.pressed) {
                    splitHandle.pressWidth = roomsPanel.width
                    return
                }
                const moved = splitHandle.pressWidth < 0
                    || Math.abs(roomsPanel.width - splitHandle.pressWidth) >= 2
                splitHandle.pressWidth = -1
                if (moved) {
                    splitHandle.lastClickMs = 0
                    return
                }
                widthSaver.dragged = false
                const now = Date.now()
                if (roomsPanel.compact) {
                    splitHandle.lastClickMs = 0
                    roomsPanel.expand()
                } else if (now - splitHandle.lastClickMs
                           <= Qt.styleHints.mouseDoubleClickInterval) {
                    splitHandle.lastClickMs = 0
                    roomsPanel.collapse()
                } else {
                    splitHandle.lastClickMs = now
                }
            }

            containmentMask: grabMask
            Item {
                id: grabMask
                x: -4
                width: 9
                height: splitHandle.height
            }

            Rectangle {
                anchors.fill: parent
                color: splitHandle.SplitHandle.pressed ? AppTheme.accent
                     : splitHandle.SplitHandle.hovered ? AppTheme.borderStrong
                                                       : AppTheme.border
                Behavior on color { ColorAnimation { duration: 90 } }
            }
            Rectangle {
                objectName: "roomListDividerFocusRing"
                visible: splitHandle.activeFocus
                x: -1
                width: 3
                height: parent.height
                color: AppTheme.focusRing
            }
        }

        SpacesRail {
            id: spacesRail
            objectName: "spacesRail"
            visible: app.settings.spacesRailVisible
            SplitView.preferredWidth: app.settings.spacesRailWidth
            SplitView.minimumWidth:
                Math.max(AppTheme.compactDensity ? 62
                                                 : app.settings.spacesRailMinWidth,
                         spacesRail.minRailWidth)
            SplitView.maximumWidth:
                Math.min(app.settings.spacesRailMaxWidth,
                         spacesRail.maxRailWidth)
            onCreateSpaceRequested: roomsPanel.startConversation("space")
            onWidthChanged: if (!SplitView.view.resizing && railWidthSaver.dragged)
                                railWidthSaver.restart()
            Connections {
                target: spacesRail.SplitView.view
                function onResizingChanged() {
                    if (spacesRail.SplitView.view.resizing)
                        railWidthSaver.dragged = true
                    else if (railWidthSaver.dragged)
                        railWidthSaver.restart()
                }
            }
            Timer {
                id: railWidthSaver
                interval: 250
                property bool dragged: false
                onTriggered: {
                    dragged = false
                    if (!spacesRail.visible || spacesRail.width <= 0)
                        return
                    // Store the dragged width, then restore the binding:
                    // SplitView writes preferredWidth while dragging, which
                    // breaks it.
                    var chosen = Math.round(spacesRail.width)
                    app.settings.spacesRailWidth = chosen
                    spacesRail.SplitView.preferredWidth = Qt.binding(
                        function() {
                            return app.settings.spacesRailWidth
                        })
                }
            }
        }

        RoomsPanel {
            id: roomsPanel
            objectName: "roomsPanel"
            visible: app.settings.roomListVisible
            SplitView.preferredWidth: app.settings.roomListWidth
            SplitView.minimumWidth:   app.settings.roomListMinWidth
            SplitView.maximumWidth:   560
            onMessageSearchRequested: messageSearchDialog.openDialog()
            onWidthChanged: if (!SplitView.view.resizing && widthSaver.dragged)
                                widthSaver.restart()
            Connections {
                target: roomsPanel.SplitView.view
                function onResizingChanged() {
                    if (roomsPanel.SplitView.view.resizing)
                        widthSaver.dragged = true
                    else if (widthSaver.dragged)
                        widthSaver.restart()
                    else
                        roomsPanel.SplitView.preferredWidth = Qt.binding(
                            function() { return app.settings.roomListWidth })
                }
            }
            Timer {
                id: widthSaver
                interval: 250
                property bool dragged: false
                onTriggered: {
                    dragged = false
                    if (!roomsPanel.visible || roomsPanel.width <= 0)
                        return
                    const chosen = Math.round(roomsPanel.width)
                    if (chosen < app.settings.roomListSnapThreshold) {
                        if (app.settings.roomListWidth > app.settings.roomListMinWidth)
                            app.settings.setRoomListExpandedWidth(app.settings.roomListWidth)
                        app.settings.roomListWidth = app.settings.roomListMinWidth
                    } else {
                        const expanded = Math.max(app.settings.roomListExpandedMinWidth,
                                                  chosen)
                        app.settings.setRoomListExpandedWidth(expanded)
                        app.settings.roomListWidth = expanded
                    }
                    roomsPanel.SplitView.preferredWidth = Qt.binding(
                        function() { return app.settings.roomListWidth })
                }
            }
        }

        TimelinePane {
            id: timelinePane
            objectName: "timelinePane"
            SplitView.fillWidth:  true
            SplitView.minimumWidth: 320
            onNewConversationRequested:
                (mode, options) => roomsPanel.startConversation(mode, options)
            onSearchAllRoomsRequested:
                (query) => messageSearchDialog.openDialog(query)
            onDiscoverRoomsRequested: roomsPanel.openDiscover("browse")
        }
    }

    Rectangle {
        id: switchingOverlay
        objectName: "accountSwitchingOverlay"
        anchors.fill: parent
        readonly property bool signingOut: !!(app.auth && app.auth.logoutPending)
        visible: app.accountSwitching || signingOut
        color: AppTheme.overlayScrim

        MouseArea { anchors.fill: parent; hoverEnabled: true }

        Rectangle {
            anchors.centerIn: parent
            width: switchingColumn.implicitWidth + AppTheme.spacing24 * 2
            height: switchingColumn.implicitHeight + AppTheme.spacing24 * 2
            radius: AppTheme.radiusCard
            color: AppTheme.surface
            border.color: AppTheme.border

            Column {
                id: switchingColumn
                anchors.centerIn: parent
                spacing: AppTheme.spacing12

                AppBusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: switchingOverlay.visible
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: switchingOverlay.signingOut ? qsTr("Signing out…")
                                                      : qsTr("Switching account…")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                }
            }
        }
    }
}
