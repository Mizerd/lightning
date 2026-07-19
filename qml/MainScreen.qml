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

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 1
            color: AppTheme.border
        }

        // ── Spaces rail ───────────────────────────────────────────────────
        SpacesRail {
            SplitView.preferredWidth: 68
            SplitView.minimumWidth:   68
            SplitView.maximumWidth:   68
        }

        // ── Rooms column ──────────────────────────────────────────────────
        RoomsPanel {
            SplitView.preferredWidth: 300
            SplitView.minimumWidth:   240
            SplitView.maximumWidth:   360
        }

        // ── Chat area ─────────────────────────────────────────────────────
        TimelinePane {
            SplitView.fillWidth:  true
            SplitView.minimumWidth: 320
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
