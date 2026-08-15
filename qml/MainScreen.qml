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
        if (app.timeline && app.timeline.setMentionStyle)
            app.timeline.setMentionStyle(accent, soft, code)
        if (app.thread && app.thread.model && app.thread.model.setMentionStyle)
            app.thread.model.setMentionStyle(accent, soft, code)
    }
    Component.onCompleted: _pushMentionStyle()
    Connections {
        target: AppTheme
        function onAccentChanged() { _pushMentionStyle() }
        function onAccentSoftChanged() { _pushMentionStyle() }
        function onCodeBlockChanged() { _pushMentionStyle() }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 1
            color: AppTheme.border
        }

        // ── Spaces rail ───────────────────────────────────────────────────
        SpacesRail {
            objectName: "spacesRail"
            SplitView.preferredWidth: 68
            SplitView.minimumWidth:   68
            SplitView.maximumWidth:   68
            onCreateSpaceRequested: roomsPanel.startConversation("space")
        }

        // ── Rooms column ──────────────────────────────────────────────────
        RoomsPanel {
            id: roomsPanel
            objectName: "roomsPanel"
            SplitView.preferredWidth: 300
            SplitView.minimumWidth:   240
            SplitView.maximumWidth:   360
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
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border

            Column {
                id: switchingColumn
                anchors.centerIn: parent
                spacing: AppTheme.spacing12

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: switchingOverlay.visible
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Switching account…")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontBody
                    font.weight: Font.DemiBold
                }
            }
        }
    }
}
