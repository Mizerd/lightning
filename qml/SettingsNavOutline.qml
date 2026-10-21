import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "On this page": the headings of the open settings page, listed under its row
// in the navigation column. The one the reader is looking at carries an accent
// mark; a click asks the owner to scroll there. The owner supplies `entries`
// ({label, anchors}) and `activeIndex` (it measures the scroll position), so
// this item is only the picture.
Item {
    id: outline

    // [{label: string, anchors: [objectName, ...]}], in page order.
    property var entries: []
    // Index of the heading in view, or -1.
    property int activeIndex: -1
    // False for every row but the open section, and while a search is showing
    // results in place of the list.
    property bool shown: false

    signal entryActivated(int index)
    // A keyboard-focused entry, so the owner can scroll it into view.
    signal entryFocused(Item entry)

    objectName: "settingsNavOutline"
    visible: shown && entries.length > 0
    Layout.fillWidth: true
    // Indented under the row's label, clear of its caret gutter.
    Layout.leftMargin: AppTheme.spacing8
    implicitHeight: visible ? column.implicitHeight + AppTheme.spacing4 : 0

    Accessible.role: Accessible.List
    Accessible.name: qsTr("On this page")

    // The rule the marks sit on.
    Rectangle {
        x: AppTheme.spacing12
        y: heading.height
        width: 1
        height: Math.max(0, column.implicitHeight - heading.height)
        color: AppTheme.stormBorder
    }

    Column {
        id: column
        width: parent.width
        spacing: 0

        Label {
            id: heading
            width: parent.width
            leftPadding: AppTheme.spacing12 + AppTheme.spacing8
            topPadding: AppTheme.spacing4
            bottomPadding: AppTheme.spacing4
            text: qsTr("On this page")
            textFormat: Text.PlainText
            color: AppTheme.stormTextMuted
            font.family: AppTheme.menuSectionFont
            font.pixelSize: AppTheme.menuSectionSize
            font.weight: AppTheme.menuSectionWeight
            font.letterSpacing: AppTheme.menuSectionTracking
            elide: Label.ElideRight
        }

        Repeater {
            model: outline.entries
            delegate: AbstractButton {
                id: entryRow
                required property var modelData
                required property int index
                readonly property bool current: outline.activeIndex === index
                objectName: "settingsNavOutlineEntry_" + index
                width: column.width
                implicitHeight: 26
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                // Paddings and the bar are physical, so under RTL they swap:
                // the active bar belongs at the START edge of the row.
                readonly property real startInset: AppTheme.spacing12 + AppTheme.spacing8
                leftPadding: entryRow.mirrored ? AppTheme.spacing8 : entryRow.startInset
                rightPadding: entryRow.mirrored ? entryRow.startInset : AppTheme.spacing8
                Accessible.role: Accessible.Button
                Accessible.name: modelData.label
                Accessible.selected: entryRow.current
                onClicked: outline.entryActivated(entryRow.index)
                onActiveFocusChanged: if (activeFocus) outline.entryFocused(entryRow)

                background: Item {
                    Rectangle {
                        x: entryRow.mirrored
                           ? parent.width - width - (AppTheme.spacing12 - 1)
                           : AppTheme.spacing12 - 1
                        width: 3
                        height: parent.height - 6
                        y: 3
                        radius: 1
                        visible: entryRow.current
                        color: AppTheme.bolt
                    }
                    Rectangle {
                        x: entryRow.mirrored ? 0 : AppTheme.spacing12 + AppTheme.spacing4
                        width: parent.width - (AppTheme.spacing12 + AppTheme.spacing4)
                        height: parent.height
                        radius: AppTheme.radiusSm
                        color: entryRow.hovered
                               ? Qt.alpha(AppTheme.selectedHover, 0.55)
                               : "transparent"
                        border.width: entryRow.visualFocus ? 2 : 0
                        border.color: AppTheme.bolt
                    }
                }
                contentItem: Label {
                    text: entryRow.modelData.label
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignVCenter
                    elide: Label.ElideRight
                    color: entryRow.current ? AppTheme.stormText
                                            : AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: entryRow.current ? AppTheme.weightStrong
                                                  : AppTheme.weightMedium
                }
            }
        }
    }
}
