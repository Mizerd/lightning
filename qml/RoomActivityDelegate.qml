import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Compact room-state annotation shared by the real timeline delegate and
// QML-facing tests. Entries are typed maps produced by TimelineModel; this
// component never reconstructs events from the summary label.
Item {
    id: root
    property string groupId: ""
    property var entries: []
    property bool expanded: false
    signal toggleRequested()

    readonly property int entryCount: entries ? entries.length : 0
    readonly property bool canExpand: entryCount > 0
    readonly property int renderedEntryCount: activityRepeater.count
    readonly property real expandedContentHeight: expandedColumn.height
    implicitHeight: visible ? activityColumn.implicitHeight : 0

    Column {
        id: activityColumn
        width: parent.width
        height: implicitHeight
        spacing: 1

        Control {
            id: summaryRow
            objectName: "stateActivitySummary"
            width: parent.width
            height: implicitHeight
            implicitHeight: summaryContent.implicitHeight + AppTheme.spacingXS * 2
            enabled: root.canExpand
            hoverEnabled: true
            focusPolicy: root.canExpand ? Qt.StrongFocus : Qt.NoFocus
            Accessible.role: Accessible.Button
            Accessible.name: summaryLabel.text
            Accessible.description: root.expanded ? qsTr("Collapse room updates")
                                                  : qsTr("Expand room updates")

            background: Rectangle {
                radius: AppTheme.radiusSm
                color: summaryRow.hovered || summaryRow.activeFocus
                       ? AppTheme.hover : "transparent"
            }
            contentItem: RowLayout {
                id: summaryContent
                spacing: AppTheme.spacingXS
                Label {
                    objectName: "stateActivityChevron"
                    visible: root.canExpand
                    text: root.expanded ? "▾" : "▸"
                    color: AppTheme.textMuted
                    font.pixelSize: 10
                }
                Label {
                    id: summaryLabel
                    Layout.fillWidth: true
                    text: root.entryCount === 0
                          ? qsTr("Room updated")
                          : root.entryCount === 1
                            ? qsTr("1 room update")
                            : qsTr("%1 room updates").arg(root.entryCount)
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    elide: Label.ElideRight
                }
            }

            TapHandler {
                enabled: root.canExpand
                onTapped: {
                    summaryRow.forceActiveFocus(Qt.MouseFocusReason)
                    root.toggleRequested()
                }
            }
            Keys.onPressed: (event) => {
                if (root.canExpand
                    && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space)) {
                    root.toggleRequested()
                    event.accepted = true
                }
            }
        }

        Column {
            id: expandedColumn
            objectName: "stateActivityExpandedContent"
            x: AppTheme.spacingM
            width: Math.max(0, parent.width - x)
            height: visible ? implicitHeight : 0
            spacing: 1
            visible: root.expanded && root.canExpand

            Repeater {
                id: activityRepeater
                objectName: "stateActivityRepeater"
                model: expandedColumn.visible ? root.entries : []
                Label {
                    required property var modelData
                    objectName: "stateActivityEntry"
                    width: expandedColumn.width
                    height: Math.max(16, implicitHeight)
                    text: modelData.description || ""
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }
            }
        }
    }
}
