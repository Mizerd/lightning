import QtQuick
import MatrixClient

// Small status pill (Verified, ACTIVE, MOD, LOUD, unread counts). `tone` picks
// the colour family; `solid` switches from the soft tint (ink at 14% with a 32%
// border) to a filled pill. `storm: true` uses the storm chip vocabulary; bolt
// is reserved for selection/verified, and `accent` uses the link tone.
Rectangle {
    id: root

    property string label: ""
    // Optional leading Material Symbols glyph.
    property string iconName: ""
    // neutral | accent | success | warning | danger | info | onAccent | bolt
    // (storm-only). onAccent is a chip on an accent-gradient card and inks in
    // accentText.
    property string tone: "neutral"
    property bool solid: false
    property bool storm: false
    property int textSize: AppTheme.textMicro

    // The yellow chip: "bolt", and "onAccent" (on an accent card where a tint
    // would vanish).
    readonly property bool _boltChip: storm && (tone === "bolt"
                                                || tone === "onAccent")
    readonly property color _base: {
        if (storm) {
            if (_boltChip) return AppTheme.bolt
            if (tone === "accent") return AppTheme.stormLink
            if (tone === "success") return AppTheme.stormSuccess
            if (tone === "warning") return AppTheme.warning
            if (tone === "danger") return AppTheme.stormDanger
            if (tone === "info") return AppTheme.info
            return AppTheme.stormTextMuted
        }
        return tone === "accent" ? AppTheme.accent
             : tone === "success" ? AppTheme.presenceOnline
             : tone === "warning" ? AppTheme.warning
             : tone === "danger" ? AppTheme.mentionBadge
             : tone === "info" ? AppTheme.info
             : tone === "onAccent" ? AppTheme.accentText
             : AppTheme.textMuted
    }
    // A soft chip's ink must clear the chip, not the card: the 14% tint lifts
    // the background toward the ink, and the muted neutral ink fails AA on most
    // palettes. stormText clears everywhere (stormTextSecondary does not). Fill
    // and border keep the muted base.
    readonly property bool _neutralSoft: tone === "neutral" && !solid
    // Other tones fail on the grounds chips are dropped on too, and legacy
    // tones worse. AppTheme.softChipInk keeps the hue and saturation and
    // adjusts lightness only until the tone clears 4.5:1 on the worst ground,
    // returning it unchanged where it already passes; fill and border keep the
    // raw tone. onAccent is excluded: its ground is an accent gradient and
    // accentText is already its ink.
    readonly property bool _tintedSoft: !solid && !_neutralSoft
                                        && tone !== "onAccent"
                                        && !_boltChip
    readonly property color _ink: {
        if (storm)
            // Ink on the bolt/solid fill: boltInk.
            return _boltChip ? AppTheme.boltInk
                 : solid ? AppTheme.boltInk
                 : _neutralSoft ? AppTheme.stormText
                 : _tintedSoft ? AppTheme.softChipInk(_base)
                 : _base
        return solid
            ? (tone === "danger" ? AppTheme.dangerText
               : tone === "accent" ? AppTheme.accentText
               : AppTheme.textPrimary)
            : _neutralSoft ? AppTheme.textPrimary
            : _tintedSoft ? AppTheme.softChipInk(_base)
            : _base
    }

    implicitWidth: chipContent.implicitWidth + 2 * AppTheme.chipPaddingH
    implicitHeight: Math.max(AppTheme.chipHeight,
                             chipContent.implicitHeight + 2 * AppTheme.spacing2)
    radius: AppTheme.chipRadius
    color: {
        if (storm)
            return _boltChip ? AppTheme.bolt
                 : solid ? _base
                 : Qt.alpha(_base, AppTheme.softChipFillAlpha)
        return solid ? _base
                     : Qt.alpha(_base, tone === "onAccent"
                                     ? 0.25 : AppTheme.softChipFillAlpha)
    }
    border.width: _boltChip || solid || (!storm && tone === "onAccent") ? 0 : 1
    border.color: Qt.alpha(_base, 0.32)

    Row {
        id: chipContent
        anchors.centerIn: parent
        spacing: AppTheme.spacing2

        Icon {
            objectName: "chipIcon"
            visible: root.iconName.length > 0
            name: root.iconName
            size: root.textSize + 1
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            // Untrusted text: never markup.
            textFormat: Text.PlainText
            objectName: "chipLabel"
            visible: root.label.length > 0
            text: root.label
            // The UI face: a status pill is a label; mono is for keycaps, code
            // and IDs.
            font.family: AppTheme.uiFont
            font.pixelSize: root.textSize
            font.weight: AppTheme.weightBold
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
