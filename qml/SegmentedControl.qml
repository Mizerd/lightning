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
    // Storm surfaces (Settings, pickers): storm selection fill and inks;
    // themed hosts (Room Information tabs) keep the default treatment.
    property bool storm: false
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
                color: {
                    if (root.storm)
                        return !segment.enabled ? AppTheme.stormTextFaint
                             : segment.selected ? AppTheme.stormText
                             : AppTheme.stormTextMuted
                    // Storm routing (2026-08-15 report): under the Storm
                    // theme the themed pair accentSoft+accentText is dark
                    // navy ink on a 14% translucent bolt — unreadable. The
                    // selected chip becomes the sanctioned SOLID bolt
                    // "current selection" moment instead, with boltInk on
                    // it (never dark-on-dark). Legacy themes keep the
                    // accent-soft chip unchanged.
                    return !segment.enabled ? AppTheme.textDisabled
                         : segment.selected ? (AppTheme.storm
                                               ? AppTheme.boltInk
                                               : AppTheme.accentText)
                         : AppTheme.textSecondary
                }
                font.family: root.storm ? AppTheme.menuFont : AppTheme.uiFont
                font.pixelSize: root.dense ? 12 : 13
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: AppTheme.radiusMd
                color: {
                    if (root.storm)
                        return segment.selected ? AppTheme.stormSelection
                             : segment.enabled && (segment.down || segment.hovered)
                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                               : "transparent"
                    return segment.selected ? (AppTheme.storm
                                               ? AppTheme.bolt
                                               : AppTheme.accentSoft)
                         : segment.enabled && (segment.down || segment.hovered)
                           ? AppTheme.hover : "transparent"
                }
                border.width: root.storm && segment.selected ? 1 : 0
                border.color: AppTheme.stormBorderStrong
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusMd + 3
                color: "transparent"
                border.color: root.storm ? AppTheme.bolt : AppTheme.focusRing
                border.width: 2
                visible: segment.visualFocus
            }
        }
    }
}
