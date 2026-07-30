import QtQuick
import QtQuick.Controls
import MatrixClient

// One coherent segmented row for mutually exclusive choices (Room
// Information tabs, GIF provider/section tabs, message-layout selector).
// Segments are flat: the selected segment carries the accent-soft chip with
// accent text; unselected segments are transparent with a soft hover tint —
// never independent outlined rectangles, never native TabButtons.
//
// model: list of { label, value, enabled?, tip? } (or plain strings, where
// the string is both label and value). `current` is the selected value;
// clicking emits activated(value) — the owner updates `current`.
Row {
    id: root

    property var model: []
    property var current
    // Compact variant for tight hosts (the 260px Settings-nav inline
    // results): smaller type and padding, same interaction and states.
    property bool dense: false
    signal activated(var value)

    spacing: 2

    Repeater {
        model: root.model
        delegate: AbstractButton {
            id: segment
            required property var modelData
            objectName: root.objectName.length > 0
                        ? root.objectName + "_" + String(segValue) : ""
            readonly property string segLabel:
                typeof modelData === "string" ? modelData
                                              : (modelData.label || "")
            readonly property var segValue:
                typeof modelData === "string" ? modelData : modelData.value
            readonly property bool selected: root.current === segValue

            enabled: typeof modelData === "string"
                     || modelData.enabled === undefined
                     || modelData.enabled === true
            implicitWidth: segText.implicitWidth + (root.dense ? 12 : 24)
            implicitHeight: root.dense ? 26 : 30
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.RadioButton
            Accessible.name: segLabel
            ToolTip.text: typeof modelData === "string"
                          ? "" : (modelData.tip || "")
            ToolTip.visible: hovered && ToolTip.text.length > 0
            ToolTip.delay: 400
            onClicked: root.activated(segValue)

            contentItem: Label {
                id: segText
                text: segment.segLabel
                color: !segment.enabled ? AppTheme.textDisabled
                       : segment.selected ? AppTheme.accentText
                       : AppTheme.textSecondary
                font.pixelSize: root.dense ? 12 : 13
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: AppTheme.radiusMd
                color: segment.selected ? AppTheme.accentSoft
                       : segment.enabled && (segment.down || segment.hovered)
                         ? AppTheme.hover : "transparent"
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusMd + 3
                color: "transparent"
                border.color: AppTheme.focusRing
                border.width: 2
                visible: segment.visualFocus
            }
        }
    }
}
