import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A segmented row for mutually exclusive choices. The selected segment is an
// accent-soft chip; others are transparent with a hover tint. model: list of {
// label, value, enabled?, tip? } (or plain strings used as both). `current` is
// the selected value; a click emits activated(value) and the owner updates
// `current`. A RowLayout rather than a Row so fitWidth can compact it; with
// fitWidth off they are equivalent.
RowLayout {
    id: root

    property var model: []
    property var current
    // Compact variant for tight hosts: smaller type and padding.
    property bool dense: false
    // Fit the row into the host's width instead of overflowing (opt-in; needed
    // in clipping hosts like the room-list column). The layout does the
    // compaction (fillWidth plus a maximumWidth of the natural width): a
    // hand-rolled scale on a Row loops, because a Row's implicitWidth follows
    // its children's assigned widths. A RowLayout's implicitWidth is the sum of
    // implicit widths and stays put, which `overflowing` relies on. The
    // trailing filler keeps segments packed at the start: a RowLayout given
    // extra width spreads its children apart.
    property bool fitWidth: false
    readonly property real segmentSpacing: 2
    readonly property bool overflowing:
        fitWidth && width > 0 && implicitWidth > width
    // Storm surfaces use storm selection fill and inks; themed hosts keep the
    // default.
    property bool storm: false
    signal activated(var value)

    spacing: root.segmentSpacing

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
            implicitHeight: root.dense ? AppTheme.buttonHeightSm
                                       : AppTheme.buttonHeight
            // Only when the row does not fit; otherwise fillWidth spreads the
            // segments.
            Layout.fillWidth: root.overflowing
            Layout.maximumWidth: implicitWidth
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            // A disabled segment offers no tip; AbstractButton keeps `hovered`
            // true while disabled, so guard explicitly.
            opacity: enabled ? 1.0 : 0.55
            Accessible.role: Accessible.RadioButton
            Accessible.name: segLabel
            ToolTip.text: typeof modelData === "string"
                          ? "" : (modelData.tip || "")
            ToolTip.visible: enabled && hovered && ToolTip.text.length > 0
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
                // Elide as a last resort; clipped glyphs look like a rendering
                // fault.
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: AppTheme.buttonRadius
                color: {
                    // A disabled segment gets its own faint field, on top of
                    // the reduced opacity.
                    if (!segment.enabled)
                        return Qt.alpha(AppTheme.borderStrong,
                                        segment.selected ? 0.60 : 0.35)
                    if (root.storm)
                        return segment.selected ? AppTheme.stormSelection
                             : (segment.down || segment.hovered)
                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                               : "transparent"
                    if (segment.selected)
                        // accentSoft under Storm too, not solid bolt: the solid
                        // fill is reserved for the primary button, and a
                        // control at its default value should not carry the
                        // loudest colour.
                        return AppTheme.accentSoft
                    return segment.down ? AppTheme.buttonGhostPressed
                         : segment.hovered ? AppTheme.buttonGhostHover
                         : "transparent"
                }
                border.width: root.storm && segment.selected ? 1 : 0
                border.color: AppTheme.stormBorderStrong
            }
            // Inset focus ring (see AppButton.qml): segments are 2px apart.
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

    // Turns surplus width into trailing space (see fitWidth).
    Item {
        Layout.fillWidth: true
        implicitWidth: 0
        implicitHeight: 0
    }
}
