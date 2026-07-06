import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.3: left sidebar — Spaces section.
// Sits above RoomsPanel in a ColumnLayout. Has its own search and
// ScrollView. Collapse toggle shrinks the panel to header-only height so
// RoomsPanel absorbs the freed space. Uses AppTheme tokens throughout.
Rectangle {
    id: root
    color: AppTheme.sidebar
    clip: true

    property bool collapsed: false

    // implicitHeight tracks the visible content so the parent ColumnLayout
    // (in MainScreen) can size correctly when the panel collapses/expands.
    implicitHeight: column.implicitHeight

    // ── Internal layout ──────────────────────────────────────────────────
    ColumnLayout {
        id: column
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: headerRow.implicitHeight + AppTheme.spacing8 * 2

            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing8
                anchors.topMargin: AppTheme.spacing8
                anchors.bottomMargin: AppTheme.spacing8
                spacing: AppTheme.spacing4

                Label {
                    text: qsTr("SPACES")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }

                Item { Layout.fillWidth: true }

                ToolButton {
                    text: root.collapsed ? "▶" : "▼"
                    font.pixelSize: 9
                    implicitWidth: 22
                    implicitHeight: 22
                    onClicked: root.collapsed = !root.collapsed

                    ToolTip {
                        text: root.collapsed ? qsTr("Expand spaces") : qsTr("Collapse spaces")
                        visible: parent.hovered
                        delay: 600
                    }
                }
            }
        }

        // ── "No spaces" compact empty state ──────────────────────────────
        // Shown when no real Matrix Spaces exist on the active account.
        Label {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            visible: !root.collapsed && (!app.spaces || !app.spaces.hasSpaces)
            text: qsTr("No spaces")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.fontSizeS
            horizontalAlignment: Text.AlignHCenter
        }

        // ── Search ───────────────────────────────────────────────────────
        TextField {
            id: spaceSearch
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing8
            Layout.rightMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing4
            visible: !root.collapsed && (app.spaces && app.spaces.hasSpaces)
            placeholderText: qsTr("Search spaces")
            font.pixelSize: AppTheme.fontSizeS
            background: Rectangle {
                color: AppTheme.inputBackground
                border.color: spaceSearch.activeFocus ? AppTheme.focusRing
                                                      : AppTheme.inputBorder
                border.width: spaceSearch.activeFocus ? 2 : 1
                radius: AppTheme.radiusSm
            }
        }

        // ── Spaces ListView ───────────────────────────────────────────────
        ListView {
            id: spaceList
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            visible: !root.collapsed && (app.spaces && app.spaces.hasSpaces)
            clip: true
            model: app.spaces
            spacing: 0

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Item {
                id: spaceItem
                width: ListView.view.width

                property bool isSelected: model.spaceId === (app.spaces ? app.spaces.activeSpaceId : "")

                // Pseudo-space rows (All rooms, Other rooms) always pass the
                // filter; real spaces are checked case-insensitively by name.
                property bool matchesFilter: {
                    var q = spaceSearch.text
                    if (q === "") return true
                    if (model.spaceId === "" || model.spaceId === "@orphans") return true
                    return model.name &&
                           model.name.toLowerCase().indexOf(q.toLowerCase()) >= 0
                }

                height: matchesFilter ? 32 : 0
                clip: true

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: AppTheme.radiusSm
                    color: isSelected ? AppTheme.selected
                         : spaceHover.hovered ? AppTheme.hover
                         : "transparent"
                    HoverHandler { id: spaceHover }
                    TapHandler {
                        onTapped: {
                            if (app.spaces)
                                app.spaces.activeSpaceId = model.spaceId
                        }
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing8
                    anchors.rightMargin: AppTheme.spacing8
                    spacing: AppTheme.spacing8

                    // "All rooms" icon
                    Label {
                        visible: model.spaceId === ""
                        text: "⊞"
                        font.pixelSize: 14
                        color: isSelected ? AppTheme.accent : AppTheme.textMuted
                    }
                    // "Other rooms" icon
                    Label {
                        visible: model.spaceId === "@orphans"
                        text: "◦"
                        font.pixelSize: 14
                        color: isSelected ? AppTheme.accent : AppTheme.textMuted
                    }
                    // Real space first-letter avatar
                    Rectangle {
                        visible: model.spaceId !== "" && model.spaceId !== "@orphans"
                        width: 22; height: 22
                        radius: AppTheme.radiusSm
                        color: isSelected ? AppTheme.accent : AppTheme.cardElevated
                        Label {
                            anchors.centerIn: parent
                            text: (model.name && model.name.length > 0)
                                  ? model.name.charAt(0).toUpperCase() : "#"
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: isSelected ? AppTheme.accentText : AppTheme.textSecondary
                        }
                    }

                    Label {
                        text: model.spaceId === ""       ? qsTr("All rooms")
                            : model.spaceId === "@orphans" ? qsTr("Other rooms")
                            : (model.name || "")
                        color: isSelected ? AppTheme.selectedText : AppTheme.textPrimary
                        font.pixelSize: AppTheme.fontSizeS
                        font.weight: isSelected ? Font.DemiBold : Font.Normal
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: model.childCount > 0
                        text: model.childCount
                        color: isSelected ? AppTheme.selectedText : AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeXS
                    }
                }
            }
        }
    }
}
