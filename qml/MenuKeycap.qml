import QtQuick
import MatrixClient

// v0.6.5 menu language (SPEC §0): the keyboard-accelerator keycap chip.
// Renders a mono text shortcut ("R", "Ctrl+K", "ESC"), an icon for the
// glyphs the bundled mono face does not carry (↵ → keyboard_return,
// ⇥ → keyboard_tab), or both ("Shift" + ↵). Shortcuts render in the
// project's cross-platform Ctrl convention, never macOS ⌘ symbols.
Rectangle {
    id: root

    // Text part of the shortcut ("R", "Ctrl+C", "Shift"). May be empty
    // when the chip is icon-only (↵).
    property string keys: ""
    // Material Symbols glyph appended after the text part.
    property string iconName: ""
    // Header-scale chips (the quick-switcher ESC) use the larger padding.
    property bool header: false
    // Chips on a selected row re-ink to the selection colours (SPEC 1j).
    property bool tinted: false

    readonly property int _padH: header ? AppTheme.keycapHeaderPaddingH
                                        : AppTheme.keycapPaddingH
    readonly property int _padV: header ? AppTheme.keycapHeaderPaddingV
                                        : AppTheme.keycapPaddingV

    implicitWidth: content.implicitWidth + 2 * _padH
    implicitHeight: content.implicitHeight + 2 * _padV
    radius: AppTheme.radiusChip
    color: AppTheme.keycapBackground
    border.width: 1
    border.color: tinted ? AppTheme.accentBorder : AppTheme.keycapBorder

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
            color: root.tinted ? AppTheme.selectedText : AppTheme.keycapText
            anchors.verticalCenter: parent.verticalCenter
        }
        Icon {
            objectName: "keycapGlyph"
            visible: root.iconName.length > 0
            name: root.iconName
            size: AppTheme.fontChip + 2
            color: root.tinted ? AppTheme.selectedText : AppTheme.keycapText
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
