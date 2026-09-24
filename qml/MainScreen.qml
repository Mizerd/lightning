import QtQuick
import QtQuick.Controls
import MatrixClient

// The main shell: SpacesRail, RoomsPanel, and TimelinePane (which hosts the
// member/thread side panel), in a SplitView.
Item {
    // Panel visibility: Ctrl+B for the room list, Ctrl+Shift+B for the rail by
    // default, both mirrored as switches in Settings -> Appearance. Sequences
    // come from ShortcutRegistry; bindingRevision is read inside each binding
    // because sequenceFor() creates no dependency.
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

    // Opens Settings. SettingsScreen declares the same action for when Settings
    // is already open (it focuses the search field). Two enabled Shortcuts on
    // one sequence make Qt fire neither, so the gates are exact complements:
    // SettingsScreen's `root.visible` is app.currentScreen === 2 (see
    // Main.qml's settingsViewLoader). The gate is needed because Shortcuts stay
    // live while MainScreen is hidden under Settings.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.openSettings")]
        }
        enabled: app.currentScreen !== 2
        onActivated: app.showSettings()
    }
    // The shortcut list is the rebinding page, so navigate there. Ungated:
    // showSettingsSection() works whether Settings is open or not.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.shortcutsHelp")]
        }
        onActivated: app.showSettingsSection("shortcuts")
    }

    // Call audio keys, window-global so they work while doing something else.
    // They follow the call bar's lane selection: the SFU lane while a group
    // call is live, the legacy 1:1 lane otherwise. With no call they do
    // nothing, while still holding their sequences.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleMute")]
        }
        onActivated: {
            if (app.groupCall.active)
                app.groupCall.toggleMicrophoneMuted()
            else if (app.calls.muteControlAvailable)
                app.calls.toggleMicrophoneMuted()
        }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleDeafen")]
        }
        onActivated: {
            if (app.groupCall.active)
                app.groupCall.toggleDeafened()
            else if (app.calls.muteControlAvailable)
                app.calls.toggleDeafened()
        }
    }
    // Camera: SFU lane only; the legacy 1:1 lane is audio-only.
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
    // Back to the room the live call is in, from anywhere including Settings.
    // Both lanes; the Voice Connected strip's Return button does the same.
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

    // Start a call in the open conversation. Gated, since this starts
    // something. canStartCall() alone is not enough: it only asks whether the
    // room has a lane, and SfuCallController::join tears down any current call,
    // so without the in-call clauses this key could end a live call. These
    // clauses are the header call button's gate verbatim (TimelinePane.qml);
    // theStartCallKeyIsGatedLikeTheCallButton keeps them in step.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.startCall")]
        }
        enabled: app.loggedIn && app.currentRoomId !== ""
                 && app.canStartCall(app.currentRoomId)
                 // canStartCall is a Q_INVOKABLE with no NOTIFY;
                 // callGateRevision re-evaluates it when RTC state lands.
                 && app.callGateRevision >= 0
                 && !app.groupCall.active
                 && (app.calls.state === CallController.Idle
                     || app.calls.state === CallController.Ended)
        onActivated: app.startCall(app.currentRoomId, false)
    }
    // Hang up on whichever lane is live, as the call bar's leave button does.
    // Inert with no call.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.leave")]
        }
        onActivated: {
            if (app.groupCall.active)
                app.groupCall.leave()
            else if (app.calls.activeRoomId !== "")
                app.calls.hangup()
        }
    }
    // Screen share, SFU lane only. requestScreenShare() opens the picker; it
    // never starts sending on a key press.
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

    // Ctrl+K quick switcher over rooms, DMs, Spaces and invites.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.quickSwitcher")]
        }
        onActivated: quickSwitcher.open()
    }
    // Ctrl+Shift+K opens the switcher in command mode.
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
        onGlobalSearchRequested: messageSearchDialog.openDialog()
    }

    // Global server-side message search (Ctrl+Shift+F).
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.messageSearch")]
        }
        enabled: app.loggedIn && app.messageSearch.supported
        onActivated: messageSearchDialog.openDialog()
    }
    // nav.newConversation: opens the existing dialog on the Room tab.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.newConversation")]
        }
        enabled: app.loggedIn
        onActivated: roomsPanel.startConversation("room")
    }
    // room.markRead: markRoomRead already sends both the public receipt and
    // m.fully_read; this only adds a key.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.markRead")]
        }
        enabled: app.loggedIn && app.currentRoomId !== ""
        onActivated: app.roomList.markRoomRead(app.currentRoomId)
    }

    // Keys for capabilities that already have pointer-driven callers; no new
    // behaviour. Global and declared here because MainScreen lives for the
    // whole session and Shortcuts match by window.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.newDirectMessage")]
        }
        enabled: app.loggedIn
        onActivated: roomsPanel.startConversation("dm")
    }
    // Mirrors the bell's gate (RoomsPanel's activityCenterButton): app.activity
    // is null on backends without an activity model.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("nav.activityCenter")]
        }
        enabled: app.loggedIn && !!app.activity
        onActivated: roomsPanel.openActivityCenter()
    }
    // Every conversation. markAllRoomsRead() is already a no-op when nothing is
    // unread.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("room.markAllRead")]
        }
        enabled: app.loggedIn
        onActivated: app.roomList.markAllRoomsRead()
    }
    // Mark unread, previously only in the room row's context menu.
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

    // Display picker for platforms without an xdg portal, declared at the shell
    // since a share can start from more than one surface. Opens itself from the
    // controller's signal; on Linux with a portal the portal shows its own.
    ScreenSharePicker {
        id: screenSharePicker
        parent: Overlay.overlay
    }

    // A Matrix room link activated anywhere resolves and opens through Discover
    // (joined rooms open directly).
    Connections {
        target: app
        function onMatrixLinkRequested(link) {
            roomsPanel.openDiscoverForLink(link)
        }
    }

    // Development-only: find a descendant by objectName. Popup content lives
    // under contentItem; a Menu/Popup child of an Item appears only in `data`.
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

    // Screenshot-demo hooks; inert in a non-demo build.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenQuickSwitcher(query) {
            if (query && query.length > 0 && query[0] === ">") {
                // openCommandMode() starts in command mode from the first
                // frame.
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

    // Mention chips render inside sanitized rich text, so the models need the
    // theme inks (AppTheme is QML-only). Re-pushed on every theme change.
    function _pushMentionStyle() {
        var accent = "" + AppTheme.accent
        var soft = "" + AppTheme.accentSoft
        var code = "" + AppTheme.codeBlock
        // The fourth argument matters: MessageHtml paints a mention of you in
        // the accent and everything else (other mentions, URLs) in the link
        // ink, and <a href> needs a colour or Qt uses its built-in blue.
        var linkInk = "" + AppTheme.link
        if (app.timeline && app.timeline.setMentionStyle)
            app.timeline.setMentionStyle(accent, soft, code, linkInk)
        if (app.thread && app.thread.model && app.thread.model.setMentionStyle)
            app.thread.model.setMentionStyle(accent, soft, code, linkInk)
    }
    Component.onCompleted: _pushMentionStyle()
    Connections {
        target: AppTheme
        function onAccentChanged() { _pushMentionStyle() }
        function onAccentSoftChanged() { _pushMentionStyle() }
        function onCodeBlockChanged() { _pushMentionStyle() }
        // Some themes change only the link ink.
        function onLinkChanged() { _pushMentionStyle() }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // The divider between columns is the 1px rule itself, so the columns
        // are adjacent and every rule in either runs up to it; any wider
        // painted band disagrees in colour with one neighbour somewhere along
        // its height. containmentMask keeps a 9px grab area without painting
        // width.
        handle: Item {
            id: splitHandle
            implicitWidth: 1

            // Every handle is live; the rail is resizable too.
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
        }

        // Spaces rail. Hideable (Ctrl+Shift+B or Settings -> Appearance); a
        // hidden SplitView child collapses along with its handle. Resizable:
        // width lets the tiles grow and the nesting regions breathe. Depth
        // stays bounded by the model (kMaxHierarchyDepth).
        SpacesRail {
            id: spacesRail
            objectName: "spacesRail"
            visible: app.settings.spacesRailVisible
            // A plain range: past the minimum the tile grows up to a ceiling,
            // then the margins take the rest. Both limits apply: the rail's
            // (what it can draw at this scale) and the setter's clamp (what is
            // storable at any scale).
            SplitView.preferredWidth: app.settings.spacesRailWidth
            SplitView.minimumWidth:
                Math.max(app.settings.spacesRailMinWidth,
                         spacesRail.minRailWidth)
            SplitView.maximumWidth:
                Math.min(app.settings.spacesRailMaxWidth,
                         spacesRail.maxRailWidth)
            onCreateSpaceRequested: roomsPanel.startConversation("space")
            // Saved after a drag only, on the falling edge of `resizing` (a
            // release produces no widthChanged; see the rooms column). Changes
            // caused by the binding itself are not written back.
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
                /// A real divider drag happened and has not been saved yet.
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
                    // Re-bound rather than assigned, so preferredWidth keeps
                    // following the setting.
                    spacesRail.SplitView.preferredWidth = Qt.binding(
                        function() {
                            return app.settings.spacesRailWidth
                        })
                }
            }
        }

        // Rooms column: hideable (Ctrl+B) and resizable, with the width
        // persisted.
        RoomsPanel {
            id: roomsPanel
            objectName: "roomsPanel"
            visible: app.settings.roomListVisible
            SplitView.preferredWidth: app.settings.roomListWidth
            SplitView.minimumWidth:   200
            SplitView.maximumWidth:   560
            // The Channels "Message Search" row opens the same dialog as
            // Ctrl+Shift+F.
            onMessageSearchRequested: messageSearchDialog.openDialog()
            // Written back only after the drag ends: SplitView reports every
            // pixel while dragging. The release produces no widthChanged, so
            // the falling edge of `resizing` (below) is what saves.
            onWidthChanged: if (!SplitView.view.resizing) widthSaver.restart()
            Connections {
                target: roomsPanel.SplitView.view
                function onResizingChanged() {
                    if (!roomsPanel.SplitView.view.resizing)
                        widthSaver.restart()
                }
            }
            Timer {
                id: widthSaver
                interval: 250
                onTriggered: {
                    if (roomsPanel.visible && roomsPanel.width > 0)
                        app.settings.roomListWidth = Math.round(roomsPanel.width)
                }
            }
        }

        // Chat area. Settings is a full view hosted by Main.qml that hides this
        // whole shell.
        TimelinePane {
            objectName: "timelinePane"
            SplitView.fillWidth:  true
            SplitView.minimumWidth: 320
            // Home's create actions reuse the room list's dialog.
            onNewConversationRequested:
                (mode, options) => roomsPanel.startConversation(mode, options)
        }
    }

    // Account switching overlay: the previous session is already detached, so
    // block interaction and say what is happening.
    Rectangle {
        id: switchingOverlay
        anchors.fill: parent
        visible: app.accountSwitching
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
                    text: qsTr("Switching account…")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                }
            }
        }
    }
}
