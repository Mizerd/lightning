import QtQuick
import MatrixClient

// v0.6.5: the small status pill shared by the redesigned surfaces —
// Verified (1p), ACTIVE (1h), MOD (1q), LOUD (1q), unread counts (1h).
// `tone` picks the semantic colour family; `solid` switches from the soft
// tint treatment (ink at 14% with a 32% border, so a chip can never drift
// from its own ink) to a filled pill.
//
// Storm skin (SPEC-storm-language §3.7): `storm: true` renders the storm chip
// vocabulary. Note what changed on 2026-08-21: storm used to fold "accent",
// "onAccent" AND "bolt" into ONE bolt pill, so ACTIVE, MOD and VERIFIED all
// came out the same yellow and the row read as a warning strip. Bolt is now
// reserved for what it means — "this is the current selection / this is
// verified" — and `accent` takes the periwinkle link tone instead.
Rectangle {
    id: root

    property string label: ""
    // Optional leading Material Symbols glyph (verified_user on 1p's chip).
    property string iconName: ""
    // neutral | accent | success | warning | danger | info
    //         | onAccent | bolt (storm-only).
    // onAccent is the chip ON an accent-gradient fill (the ACTIVE card): it
    // inks in accentText so bright-accent themes with dark ink stay correct.
    property string tone: "neutral"
    property bool solid: false
    property bool storm: false
    property int textSize: AppTheme.textMicro

    // THE yellow chip. Deliberately narrow: "bolt" asks for it, and
    // "onAccent" keeps it because that chip is painted on an accent card
    // where a translucent tint would disappear.
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
    readonly property color _ink: {
        if (storm)
            // Ink ON the bolt/solid fill, not the panel ink — boltInk
            // (Storm: deep canvas navy; legacy: accentText) stays readable
            // once bolt/the solid tone routes to each legacy theme's own
            // accent.
            return _boltChip ? AppTheme.boltInk
                 : solid ? AppTheme.boltInk
                 : _base
        return solid
            ? (tone === "danger" ? AppTheme.dangerText
               : tone === "accent" ? AppTheme.accentText
               : AppTheme.textPrimary)
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
                 : Qt.alpha(_base, 0.14)
        return solid ? _base
                     : Qt.alpha(_base, tone === "onAccent" ? 0.25 : 0.14)
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
            objectName: "chipLabel"
            visible: root.label.length > 0
            text: root.label
            // The UI face, not the mono one. A status pill is a LABEL, not a
            // machine identifier: mono belongs to keycaps, code and Matrix
            // ids (MenuKeycap / CodeBlock keep it). A row of tracked mono
            // caps beside sentence-case UI text is the "font looks out of
            // place" the 2026-08-21 report is about.
            font.family: AppTheme.uiFont
            font.pixelSize: root.textSize
            font.weight: AppTheme.weightBold
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
