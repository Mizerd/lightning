import QtQuick
import MatrixClient

// Keyboard-accelerator keycap chip: mono text ("R", "Ctrl+K", "ESC"), an icon
// for glyphs the mono face lacks (↵ → keyboard_return, ⇥ → keyboard_tab), or
// both. Always the Ctrl convention, never macOS ⌘. Storm skin: resting chips
// are muted mono with a stormBorderStrong border; `active` flips to a bolt
// fill; `danger` inks stormDanger. The room-list search hint sets `storm:
// false` to follow the user's theme.
Rectangle {
    id: root

    // Text part ("R", "Ctrl+C", "Shift"); may be empty for icon-only chips.
    property string keys: ""
    // Material Symbols glyph after the text.
    property string iconName: ""
    // Header-scale chips (the quick-switcher ESC) use larger padding.
    property bool header: false
    // Storm treatment by default; false for keycaps on theme-following
    // surfaces.
    property bool storm: true
    // The chip on the selected row: bolt fill, panel ink.
    property bool active: false
    // Danger-group chip: stormDanger ink and border.
    property bool danger: false
    // Older alias kept for hosts that set it; equivalent to `active` under
    // Storm.
    property bool tinted: false

    readonly property bool _hot: active || tinted

    readonly property int _padH: header ? AppTheme.keycapHeaderPaddingH
                                        : AppTheme.keycapPaddingH
    readonly property int _padV: header ? AppTheme.keycapHeaderPaddingV
                                        : AppTheme.keycapPaddingV

    readonly property color _ink: {
        if (!storm)
            return tinted ? AppTheme.selectedText : AppTheme.keycapText
        // Ink on the bolt fill: boltInk.
        if (_hot) return AppTheme.boltInk
        if (danger) return AppTheme.stormDanger
        return AppTheme.stormTextMuted
    }

    implicitWidth: content.implicitWidth + 2 * _padH
    implicitHeight: content.implicitHeight + 2 * _padV
    radius: AppTheme.radiusChip
    color: storm ? (_hot ? AppTheme.bolt : "transparent")
                 : AppTheme.keycapBackground
    border.width: 1
    border.color: {
        if (!storm)
            return tinted ? AppTheme.accentBorder : AppTheme.keycapBorder
        if (_hot) return AppTheme.bolt
        if (danger) return AppTheme.stormDangerBorder
        return AppTheme.stormBorderStrong
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: AppTheme.spacing2

        Text {
            objectName: "keycapLabel"
            visible: root.keys.length > 0
            text: root.keys
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontChip
            font.weight: Font.Medium
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
        Icon {
            objectName: "keycapGlyph"
            visible: root.iconName.length > 0
            name: root.iconName
            size: AppTheme.fontChip + 2
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
