import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.5.4: 3-column navigation layout.
//   Column 1 — SpacesRail (56 px fixed, hidden when no Matrix Spaces present)
//   Column 2 — RoomsPanel (240–360 px, includes user footer)
//   Column 3 — TimelinePane (fills remaining width)
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
        // Fixed 56 px wide; auto-hides when there are no Matrix Spaces.
        SpacesRail {
            SplitView.preferredWidth: 56
            SplitView.minimumWidth:   56
            SplitView.maximumWidth:   56
        }

        // ── Rooms column ──────────────────────────────────────────────────
        RoomsPanel {
            SplitView.preferredWidth: 260
            SplitView.minimumWidth:   200
            SplitView.maximumWidth:   360
        }

        // ── Chat area ─────────────────────────────────────────────────────
        TimelinePane {
            SplitView.fillWidth:  true
            SplitView.minimumWidth: 320
        }
    }
}
