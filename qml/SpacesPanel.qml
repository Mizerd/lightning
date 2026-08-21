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
                    text: qsTr("Spaces")
                    color: AppTheme.sectionLabelColor
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                }

                Item { Layout.fillWidth: true }

                // Was a bare Basic ToolButton painting a geometric-shapes
                // Unicode triangle at 9px — a half-opacity stock control and
                // the only chevron in the app that was not a Material
                // Symbols glyph. Same IconButton every other collapse
                // affordance uses, with the shared attached tooltip.
                IconButton {
                    implicitWidth: 24
                    implicitHeight: 24
                    radius: AppTheme.radiusSm
                    iconName: root.collapsed ? "chevron_right" : "expand_more"
                    iconSize: 18
                    Accessible.name: root.collapsed ? qsTr("Expand spaces")
                                                    : qsTr("Collapse spaces")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.collapsed = !root.collapsed
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
            font.pixelSize: AppTheme.textMeta
            horizontalAlignment: Text.AlignHCenter
        }

        // ── Search ───────────────────────────────────────────────────────
        // The shared field, not a hand-rolled TextField: the local copy set
        // its own background but never `color`, `placeholderTextColor`,
        // `selectionColor` or `selectedTextColor`, so its ink came from the
        // OS palette rather than from the theme.
        AppTextField {
            id: spaceSearch
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing8
            Layout.rightMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing4
            visible: !root.collapsed && (app.spaces && app.spaces.hasSpaces)
            placeholderText: qsTr("Search spaces")
            Accessible.name: qsTr("Search spaces")
            searchIcon: true
            clearButton: true
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

            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

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

                    // Pseudo-row glyphs. Both were Labels carrying literal
                    // characters — one of them the EMPTY string, so the
                    // "All rooms" row had no glyph at all and its 14px slot
                    // silently collapsed.
                    Icon {
                        visible: model.spaceId === ""
                                 || model.spaceId === "@orphans"
                        name: model.spaceId === "" ? "home" : "workspaces"
                        size: 16
                        color: isSelected ? AppTheme.accent : AppTheme.textMuted
                    }
                    // The shared Avatar, not a local initials disc: it
                    // owns the identity palette, the mediaCached wiring and
                    // the baked "|shape:" mask, and the hand-rolled copy
                    // here painted one flat cardElevated square for every
                    // Space — no identity colour at all.
                    Avatar {
                        visible: model.spaceId !== "" && model.spaceId !== "@orphans"
                        size: 22
                        circle: false
                        squareRadius: AppTheme.radiusSm
                        labelSize: 11
                        name: model.name || ""
                        colorKey: model.spaceId || ""
                        mxc: model.avatarUrl || ""
                    }

                    Label {
                        text: model.spaceId === ""       ? qsTr("All rooms")
                            : model.spaceId === "@orphans" ? qsTr("Other rooms")
                            : (model.name || "")
                        color: isSelected ? AppTheme.selectedText : AppTheme.textPrimary
                        font.pixelSize: AppTheme.textBody
                        font.weight: isSelected ? AppTheme.weightStrong
                                                : AppTheme.weightBody
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: model.childCount > 0
                        text: model.childCount
                        color: isSelected ? AppTheme.selectedText : AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }
        }
    }
}
