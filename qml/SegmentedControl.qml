import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

RowLayout {
    id: root

    property var model: []
    property var current
    property list<Item> keyForwardTargets: []
    property bool dense: false
    property bool fitWidth: false
    readonly property real segmentSpacing: 2
    readonly property bool overflowing:
        fitWidth && width > 0 && implicitWidth > width
    property bool storm: false
    signal activated(var value)

    spacing: root.segmentSpacing

    Repeater {
        model: root.model
        delegate: AbstractButton {
            id: segment
            Keys.forwardTo: root.keyForwardTargets
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
            implicitHeight: root.dense ? AppTheme.buttonHeightSm
                                       : AppTheme.buttonHeight
            Layout.fillWidth: root.overflowing
            Layout.maximumWidth: implicitWidth
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            opacity: enabled ? 1.0 : 0.55
            Accessible.role: Accessible.RadioButton
            Accessible.name: segLabel
            ToolTip.text: typeof modelData === "string"
                          ? "" : (modelData.tip || "")
            ToolTip.visible: enabled && hovered && ToolTip.text.length > 0
            ToolTip.delay: 400
            onClicked: root.activated(segValue)
            Keys.onReturnPressed: (event) => segment._selectFromKey(event)
            Keys.onEnterPressed: (event) => segment._selectFromKey(event)
            function _selectFromKey(event) {
                if (event.isAutoRepeat) {
                    event.accepted = true
                    return
                }
                if (!segment.enabled)
                    return
                event.accepted = true
                if (typeof segment.click === "function")
                    segment.click()
                else
                    segment.clicked()
            }

            contentItem: Label {
                id: segText
                text: segment.segLabel
                color: {
                    if (root.storm)
                        return !segment.enabled ? AppTheme.stormTextFaint
                             : segment.selected ? AppTheme.stormText
                             : AppTheme.stormTextMuted
                    // accentSoft is a tint of the surface, so its ink must be a
                    // surface ink (selectedText); accentText is for solid
                    // accent fills and is unreadable on the tint in several
                    // themes. ThemeTokensTest pins the contrast. Storm keeps
                    // its solid-bolt treatment.
                    return !segment.enabled ? AppTheme.textDisabled
                         : segment.selected ? (AppTheme.storm
                                               ? AppTheme.stormText
                                               : AppTheme.selectedText)
                         : AppTheme.textSecondary
                }
                font.family: root.storm ? AppTheme.menuFont : AppTheme.uiFont
                font.pixelSize: root.dense ? AppTheme.textMeta
                                           : AppTheme.textBody
                font.weight: AppTheme.weightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: AppTheme.buttonRadius
                color: {
                    if (!segment.enabled)
                        return Qt.alpha(AppTheme.borderStrong,
                                        segment.selected ? 0.60 : 0.35)
                    if (root.storm)
                        return segment.selected ? AppTheme.stormSelection
                             : (segment.down || segment.hovered)
                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                               : "transparent"
                    if (segment.selected)
                        return AppTheme.accentSoft
                    return segment.down ? AppTheme.buttonGhostPressed
                         : segment.hovered ? AppTheme.buttonGhostHover
                         : "transparent"
                }
                border.width: root.storm && segment.selected ? 1 : 0
                border.color: AppTheme.stormBorderStrong
            }
            Rectangle {
                anchors.fill: parent
                radius: AppTheme.buttonRadius
                color: "transparent"
                border.color: {
                    if (segment.selected && AppTheme.storm && !root.storm)
                        return AppTheme.boltInk
                    return root.storm ? AppTheme.bolt : AppTheme.focusRing
                }
                border.width: 2
                visible: segment.visualFocus
            }
        }
    }

    Item {
        Layout.fillWidth: true
        implicitWidth: 0
        implicitHeight: 0
    }
}
