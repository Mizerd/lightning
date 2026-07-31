import QtQuick
import MatrixClient

// v0.6.5 menu language (SPEC §0): the keyboard-accelerator keycap chip.
// Renders a mono text shortcut ("R", "Ctrl+K", "ESC"), an icon for the
// glyphs the bundled mono face does not carry (↵ → keyboard_return,
// ⇥ → keyboard_tab), or both ("Shift" + ↵). Shortcuts render in the
// project's cross-platform Ctrl convention, never macOS ⌘ symbols.
//
// Storm skin (SPEC-storm-language §3.2): resting chips are mono muted ink
// with a 1px stormBorderStrong border; the chip on the selected/highlighted
// row flips to a bolt fill with panel ink (`active`); danger-group chips ink
// stormDanger with a 30%-alpha border. The room-list search hint opts out
// (`storm: false`) — that column renders the user's theme.
Rectangle {
    id: root

    // Text part of the shortcut ("R", "Ctrl+C", "Shift"). May be empty
    // when the chip is icon-only (↵).
    property string keys: ""
    // Material Symbols glyph appended after the text part.
    property string iconName: ""
    // Header-scale chips (the quick-switcher ESC) use the larger padding.
    property bool header: false
    // Storm menu language (default); false renders the themed treatment for
    // keycaps hosted on theme-following surfaces (room-list search).
    property bool storm: true
    // The chip on the selected/highlighted row: bolt fill, panel ink.
    property bool active: false
    // Danger-group chip: stormDanger ink and border.
    property bool danger: false
    // Pre-Storm alias (selected-row re-ink, SPEC 1j); kept for hosts that
    // still set it — equivalent to `active` under Storm.
    property bool tinted: false

    readonly property bool _hot: active || tinted

    readonly property int _padH: header ? AppTheme.keycapHeaderPaddingH
                                        : AppTheme.keycapPaddingH
    readonly property int _padV: header ? AppTheme.keycapHeaderPaddingV
                                        : AppTheme.keycapPaddingV

    readonly property color _ink: {
        if (!storm)
            return tinted ? AppTheme.selectedText : AppTheme.keycapText
        // Ink on the bolt fill, not the panel ink — boltInk stays readable
        // once bolt routes to each legacy theme's own accent.
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
