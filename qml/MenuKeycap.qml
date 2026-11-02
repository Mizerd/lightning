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

    property string keys: ""
    property string iconName: ""
    property bool header: false
    property bool storm: true
    property bool active: false
    property bool danger: false
    property bool tinted: false

    readonly property bool _hot: active || tinted

    readonly property int _padH: header ? AppTheme.keycapHeaderPaddingH
                                        : AppTheme.keycapPaddingH
    readonly property int _padV: header ? AppTheme.keycapHeaderPaddingV
                                        : AppTheme.keycapPaddingV

    readonly property color _ink: {
        if (!storm)
            return tinted ? AppTheme.selectedText : AppTheme.keycapText
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
