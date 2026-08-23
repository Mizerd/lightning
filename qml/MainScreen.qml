import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.7 design shell: classic four-pane layout (design option 1a).
//   Column 1 — SpacesRail (68 px, always visible: home, Spaces, settings,
//              account avatar with the switcher popover)
//   Column 2 — RoomsPanel (300 px preferred)
//   Column 3 — TimelinePane (fills; hosts the member/thread side panel)
// E2EE / SAS / recovery / backend behaviour is unchanged.
Item {
    // Panel visibility. Ctrl+B for the room list and Ctrl+Shift+B for the
    // spaces rail — the editor convention, and both are mirrored as switches
    // in Settings -> Appearance so neither can be turned off and then be
    // impossible to find again.
    Shortcut {
        sequences: ["Ctrl+B"]
        onActivated: app.settings.roomListVisible = !app.settings.roomListVisible
    }
    Shortcut {
        sequences: ["Ctrl+Shift+B"]
        onActivated:
            app.settings.spacesRailVisible = !app.settings.spacesRailVisible
    }
    // v0.6.1: Ctrl+K quick switcher over rooms / DMs / Spaces / invites.
    Shortcut {
        sequences: ["Ctrl+K"]
        onActivated: quickSwitcher.open()
    }
    // v0.6.5 (SPEC 1k): Ctrl+Shift+K opens the switcher straight into command
    // mode (the declarative action list) instead of requiring the user to
    // type ">" first.
    Shortcut {
        sequences: ["Ctrl+Shift+K"]
        onActivated: quickSwitcher.openCommandMode()
    }
    QuickSwitcher {
        id: quickSwitcher
        onDiscoverRequested: (startMode) => roomsPanel.openDiscover(startMode)
        onGlobalSearchRequested: messageSearchDialog.openDialog()
    }

    // v0.7.x: global server-side message search (Ctrl+Shift+F).
    Shortcut {
        sequences: ["Ctrl+Shift+F"]
        enabled: app.loggedIn && app.messageSearch.supported
        onActivated: messageSearchDialog.openDialog()
    }
    MessageSearchDialog {
        id: messageSearchDialog
        parent: Overlay.overlay
    }

    // v0.7.x: a Matrix room link activated anywhere in the app resolves and
    // opens through the Discover dialog (joined rooms auto-open from it).
    Connections {
        target: app
        function onMatrixLinkRequested(link) {
            roomsPanel.openDiscoverForLink(link)
        }
    }

    // Development-only: locate a descendant by objectName. Popup content
    // (QuickSwitcher's queryField) lives under `contentItem`, not directly
    // in `children`/`data`, so this checks that first; a Menu/Popup child of
    // a plain Item (not relevant here, but kept for a uniform helper) would
    // only ever appear in `data`, never `children`.
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

    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenQuickSwitcher(query) {
            if (query && query.length > 0 && query[0] === ">") {
                // openCommandMode() sets commandMode = true from the very
                // first frame (see QuickSwitcher.qml), unlike typing ">"
                // into an already-open plain switcher.
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

    // Mention chips render inside sanitized rich text, so the models need
    // the current theme ink (AppTheme is QML-only). Re-pushed on every
    // theme change; the models re-announce FormattedBodyRole themselves.
    function _pushMentionStyle() {
        var accent = "" + AppTheme.accent
        var soft = "" + AppTheme.accentSoft
        var code = "" + AppTheme.codeBlock
        // The FOURTH argument is load-bearing. MessageHtml paints a mention
        // of YOU in the accent and everything else — other people's mentions
        // and external URLs — in the link ink, and it also needs a colour for
        // <a href> because Qt's rich text otherwise falls back to its
        // built-in #0000ff. Omitting it collapses both onto the accent, so
        // under Storm every link rendered in bolt yellow and a mention of
        // someone else became colour-identical to a mention of you.
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
        // Without this a theme change that moves ONLY the link ink (several
        // do — Storm's link and accent are unrelated colours) would leave the
        // models on the previous theme's link colour.
        function onLinkChanged() { _pushMentionStyle() }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // The divider between columns. It used to BE the 1px painted line,
        // so the grab target was one pixel wide and nothing under the
        // pointer ever suggested the columns could be resized. The handle is
        // now a 5px transparent band (the hit area) carrying a 1px rule
        // (the drawn line) that lights on hover and takes the accent while
        // dragging — the line's own weight is unchanged.
        handle: Item {
            id: splitHandle
            implicitWidth: 5
            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height
                color: splitHandle.SplitHandle.pressed ? AppTheme.accent
                     : splitHandle.SplitHandle.hovered ? AppTheme.borderStrong
                                                       : AppTheme.border
                Behavior on color { ColorAnimation { duration: 90 } }
            }
        }

        // ── Spaces rail ───────────────────────────────────────────────────
        // Hideable (Ctrl+Shift+B, or Settings -> Appearance): a tester on
        // Windows asked for every panel to be hideable "screen real estate
        // wise". A SplitView child collapses when it is not visible, and its
        // handle goes with it.
        SpacesRail {
            objectName: "spacesRail"
            visible: app.settings.spacesRailVisible
            SplitView.preferredWidth: 68
            SplitView.minimumWidth:   68
            SplitView.maximumWidth:   68
            onCreateSpaceRequested: roomsPanel.startConversation("space")
        }

        // ── Rooms column ──────────────────────────────────────────────────
        // Hideable (Ctrl+B) and resizable, with the width persisted. The
        // range used to be 240-360 and was thrown away on exit, so a person
        // who wanted a wide list re-dragged it every launch.
        RoomsPanel {
            id: roomsPanel
            objectName: "roomsPanel"
            visible: app.settings.roomListVisible
            SplitView.preferredWidth: app.settings.roomListWidth
            SplitView.minimumWidth:   200
            SplitView.maximumWidth:   560
            // Written back only when the user let go: SplitView reports every
            // intermediate pixel while dragging, and persisting each one would
            // be one QSettings write per mouse move.
            //
            // Which is why the RELEASE needs its own trigger, and why the
            // width was in fact never saved at all. `resizing` goes true on
            // the first drag move and false on release — and release does not
            // move anything, so it produces no widthChanged. Every
            // intermediate pixel was correctly skipped, the final width was
            // never offered, and the setting kept whatever the window's first
            // layout happened to put there. The falling edge of `resizing` is
            // the one moment that matters.
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

        // ── Chat area ─────────────────────────────────────────────────────
        // Settings is a FULL application view hosted by Main.qml: it hides
        // this whole shell (rail, room list, timeline, composer) instead of
        // swapping only the center region.
        TimelinePane {
            objectName: "timelinePane"
            SplitView.fillWidth:  true
            SplitView.minimumWidth: 320
            // Home surface create actions reuse the room list's dialog.
            onNewConversationRequested:
                (mode, options) => roomsPanel.startConversation(mode, options)
        }
    }

    // ── Account switching overlay ─────────────────────────────────────────
    // While a switch is in flight the previous session is already detached;
    // block interaction and say what is happening instead of showing a
    // half-empty shell.
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
