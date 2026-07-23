import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

// Development-only screenshot-demo control panel. Loaded by Main.qml ONLY when
// app.screenshotDemoActive, so it never exists in a release binary. A floating
// overlay (no layout space): a compact collapsed pill that expands into
// scenario / account / room / theme / appearance / window-size selectors,
// toggles, and one-click reset / navigation actions. Ctrl+Shift+D (Main.qml)
// toggles app.demo.controlsVisible to hide / restore the whole thing — hidden
// leaves no overlay and no margin.
Item {
    id: root
    objectName: "demoControlPanel"
    anchors.fill: parent
    z: 100
    // Hidden entirely (no overlay, no gap) when controls are hidden or there is
    // no demo controller (defensive; app.demo is non-null in a demo build).
    visible: app.screenshotDemoActive && !!app.demo && app.demo.controlsVisible

    property bool expanded: false
    readonly property var demo: app.demo

    // C++ cannot resize the ApplicationWindow, so the controller asks us to.
    // Imperative assignment (not a binding) so the user can still resize after.
    // `Window.window` is an attached property and must be read off an Item
    // (root), never off the non-Item Connections object.
    Connections {
        target: app.demo
        enabled: !!app.demo
        function onWindowSizeRequested(w, h) {
            var win = root.Window.window
            if (!win)
                return
            win.width = Math.max(w, win.minimumWidth || 0)
            win.height = Math.max(h, win.minimumHeight || 0)
        }
    }

    Rectangle {
        id: card
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 12
        anchors.rightMargin: 12
        width: root.expanded ? 340 : collapsed.implicitWidth + 28
        height: root.expanded ? expandedCol.implicitHeight + 28 : 36
        radius: 14
        color: AppTheme.cardElevated
        border.color: AppTheme.border
        border.width: 1

        // ── Collapsed pill ───────────────────────────────────────────────
        RowLayout {
            id: collapsed
            visible: !root.expanded
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 10
            spacing: 8
            Rectangle { width: 8; height: 8; radius: 4; color: AppTheme.accent
                        Layout.alignment: Qt.AlignVCenter }
            Label {
                text: root.demo
                      ? qsTr("%1 · %2").arg(root.demo.currentAccountName)
                          .arg(root.demo.currentScenario)
                      : qsTr("Screenshot Demo")
                color: AppTheme.textPrimary
                font.pixelSize: 12; font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            ToolButton {
                text: "⤢"
                onClicked: root.expanded = true
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 24; implicitHeight: 24
                ToolTip.visible: hovered; ToolTip.text: qsTr("Expand demo controls")
            }
        }

        // ── Expanded panel ───────────────────────────────────────────────
        ColumnLayout {
            id: expandedCol
            visible: root.expanded
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Rectangle { width: 8; height: 8; radius: 4; color: AppTheme.accent
                            Layout.alignment: Qt.AlignVCenter }
                Label {
                    text: qsTr("Screenshot Demo")
                    color: AppTheme.textPrimary
                    font.pixelSize: 13; font.weight: Font.Bold
                    Layout.fillWidth: true
                }
                ToolButton { text: "▾"; implicitWidth: 24; implicitHeight: 24
                    onClicked: root.expanded = false
                    ToolTip.visible: hovered; ToolTip.text: qsTr("Collapse") }
                ToolButton { text: "✕"; implicitWidth: 24; implicitHeight: 24
                    onClicked: if (root.demo) root.demo.setControlsVisible(false)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Hide (Ctrl+Shift+D restores)") }
            }

            // Status line.
            Label {
                Layout.fillWidth: true
                visible: !!root.demo
                text: root.demo
                      ? qsTr("Demo account: %1\nScenario: %2\nSize: %3")
                          .arg(root.demo.currentAccountName)
                          .arg(root.demo.currentScenario)
                          .arg(root.demo.currentSizeLabel)
                      : ""
                color: AppTheme.textMuted
                font.pixelSize: 11
                lineHeight: 1.2
            }

            component FieldLabel: Label {
                color: AppTheme.textSecondary
                font.pixelSize: 11; font.weight: Font.DemiBold
            }

            FieldLabel { text: qsTr("Scenario") }
            ComboBox {
                id: scenarioBox
                Layout.fillWidth: true
                model: root.demo ? root.demo.scenarios : []
                textRole: "title"
                valueRole: "id"
                currentIndex: root.demo
                    ? Math.max(0, indexOfValue(root.demo.currentScenario)) : 0
                onActivated: if (root.demo) root.demo.activateScenario(currentValue)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    FieldLabel { text: qsTr("Account") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.demo ? root.demo.accounts : []
                        textRole: "name"; valueRole: "id"
                        currentIndex: root.demo
                            ? Math.max(0, indexOfValue(root.demo.currentAccount)) : 0
                        onActivated: if (root.demo) root.demo.setAccount(currentValue)
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    FieldLabel { text: qsTr("Room") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.demo ? root.demo.currentRooms : []
                        textRole: "name"; valueRole: "id"
                        currentIndex: root.demo
                            ? Math.max(0, indexOfValue(root.demo.currentRoom)) : 0
                        onActivated: if (root.demo) root.demo.setRoom(currentValue)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    FieldLabel { text: qsTr("Theme") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.demo ? root.demo.themes : []
                        textRole: "name"; valueRole: "id"
                        currentIndex: root.demo
                            ? Math.max(0, indexOfValue(root.demo.currentTheme)) : 0
                        onActivated: if (root.demo) root.demo.setTheme(currentValue)
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    FieldLabel { text: qsTr("Appearance") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.demo ? root.demo.appearances : []
                        textRole: "name"; valueRole: "id"
                        onActivated: if (root.demo) root.demo.setAppearance(currentValue)
                    }
                }
            }

            FieldLabel { text: qsTr("Window size") }
            ComboBox {
                Layout.fillWidth: true
                model: root.demo ? root.demo.windowSizes : []
                textRole: "label"; valueRole: "id"
                onActivated: if (root.demo) root.demo.setWindowSize(currentValue)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Switch {
                    text: qsTr("Typing")
                    checked: !!root.demo && root.demo.typingEnabled
                    onToggled: if (root.demo) root.demo.setTypingEnabled(checked)
                }
                Switch {
                    text: qsTr("Unread")
                    checked: !!root.demo && root.demo.unreadBadgesEnabled
                    onToggled: if (root.demo) root.demo.setUnreadBadgesEnabled(checked)
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 8; rowSpacing: 6
                Button { Layout.fillWidth: true; text: qsTr("Open Settings")
                    onClicked: if (root.demo) root.demo.openSettings() }
                Button { Layout.fillWidth: true; text: qsTr("Account switcher")
                    onClicked: if (root.demo) root.demo.openAccountSwitcher() }
                Button { Layout.fillWidth: true; text: qsTr("Open thread")
                    onClicked: if (root.demo) root.demo.openThreadPanel() }
                Button { Layout.fillWidth: true; text: qsTr("Reset scenario")
                    onClicked: if (root.demo) root.demo.resetScenario() }
                Button { Layout.fillWidth: true; text: qsTr("Reset all")
                    onClicked: if (root.demo) root.demo.resetAllDemoState() }
                Button { Layout.fillWidth: true; text: qsTr("Hide controls")
                    onClicked: if (root.demo) root.demo.setControlsVisible(false) }
            }
        }
    }
}
