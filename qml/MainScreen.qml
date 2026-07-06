import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.3: split sidebar — SpacesPanel (top) + RoomsPanel (bottom) +
// bottom-left Settings gear footer. TimelinePane fills the remainder.
// All existing openRoom / E2EE / SAS / recovery behaviour is unchanged;
// only the sidebar structure is new.
Item {
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 1
            color: AppTheme.border
        }

        // ── Left sidebar ──────────────────────────────────────────────────
        Rectangle {
            id: sidebar
            SplitView.preferredWidth: 300
            SplitView.minimumWidth: 240
            SplitView.maximumWidth: 360
            color: AppTheme.sidebar

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // ── Spaces panel ──────────────────────────────────────────
                // No Layout.fillHeight → driven by SpacesPanel.implicitHeight
                // which collapses to ~40 px when toggled. A maximumHeight cap
                // prevents it from eating more than 42 % of the sidebar when
                // there are many spaces.
                SpacesPanel {
                    id: spacesPanel
                    Layout.fillWidth: true
                    Layout.maximumHeight: Math.min(280, sidebar.height * 0.42)
                }

                // Separator shown only when spaces are present
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: AppTheme.separator
                    visible: app.spaces && app.spaces.hasSpaces
                }

                // ── Rooms panel ───────────────────────────────────────────
                RoomsPanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                // ── Bottom footer — Settings gear ─────────────────────────
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: footerRow.implicitHeight + AppTheme.spacing8 * 2
                    color: AppTheme.sidebar

                    // Top border line
                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: AppTheme.separator
                    }

                    RowLayout {
                        id: footerRow
                        anchors.fill: parent
                        anchors.margins: AppTheme.spacing8
                        spacing: AppTheme.spacing4

                        Item { Layout.fillWidth: true }

                        // Gear icon — opens the Settings screen.
                        // The top-right Settings toolbar button in Main.qml
                        // remains as a fallback for this release.
                        ToolButton {
                            id: gearBtn
                            text: "⚙"
                            font.pixelSize: 18
                            implicitWidth: 36
                            implicitHeight: 36
                            onClicked: app.showSettings()

                            ToolTip {
                                text: qsTr("Settings")
                                visible: gearBtn.hovered
                                delay: 500
                            }
                        }
                    }
                }
            }
        }

        // ── Chat area ─────────────────────────────────────────────────────
        TimelinePane {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }
    }
}
