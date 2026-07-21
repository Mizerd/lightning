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
    QuickSwitcher { id: quickSwitcher }

    // Mention chips render inside sanitized rich text, so the models need
    // the current theme ink (AppTheme is QML-only). Re-pushed on every
    // theme change; the models re-announce FormattedBodyRole themselves.
    function _pushMentionStyle() {
        var accent = "" + AppTheme.accent
        var soft = "" + AppTheme.accentSoft
        if (app.timeline && app.timeline.setMentionStyle)
            app.timeline.setMentionStyle(accent, soft)
        if (app.thread && app.thread.model && app.thread.model.setMentionStyle)
            app.thread.model.setMentionStyle(accent, soft)
    }
    Component.onCompleted: _pushMentionStyle()
    Connections {
        target: AppTheme
        function onAccentChanged() { _pushMentionStyle() }
        function onAccentSoftChanged() { _pushMentionStyle() }
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
            onNewConversationRequested: (mode) => roomsPanel.startConversation(mode)
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
