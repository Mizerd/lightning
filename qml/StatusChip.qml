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
    // A SOFT CHIP'S INK MUST CLEAR THE CHIP, NOT THE CARD, AND FOR THE
    // NEUTRAL TONE IT DID NOT. The soft fill is 14% of the tone's own
    // colour, which lifts the background TOWARDS the ink — so the label
    // measures worse on its own pill than it does on the surface behind it,
    // and `neutral` starts from the MUTED text ink, the dimmest one there
    // is. Measured 2026-09-20 over all eleven palettes, ink on the
    // composited fill: on `stormPanel` (what SettingsCard paints since
    // fea70c63) eight of eleven fail 4.5:1 AA for the textMicro label —
    // Deep Teal 3.65, Nordic 3.68, Indigo Night 3.77, Warm 3.88, Midnight
    // 3.96, Purple Dusk 4.13, Lightning Dark 4.12, Graphite 4.24, Storm
    // 4.36 — and on `stormCanvas` three more do.
    //
    // `stormTextSecondary` is the obvious next step up and it is NOT the
    // fix: on Indigo Night it is #a4a6b8 against a muted #9e9ba6 and lands
    // at 4.28, on Warm at 4.26. `stormText` clears on every palette with
    // room to spare (worst: Nordic 6.80). The FILL and the BORDER keep the
    // muted base, so the pill still reads neutral-grey; only its label
    // steps up to the ink a label on its own surface needs.
    readonly property bool _neutralSoft: tone === "neutral" && !solid
    readonly property color _ink: {
        if (storm)
            // Ink ON the bolt/solid fill, not the panel ink — boltInk
            // (Storm: deep canvas navy; legacy: accentText) stays readable
            // once bolt/the solid tone routes to each legacy theme's own
            // accent.
            return _boltChip ? AppTheme.boltInk
                 : solid ? AppTheme.boltInk
                 : _neutralSoft ? AppTheme.stormText
                 : _base
        return solid
            ? (tone === "danger" ? AppTheme.dangerText
               : tone === "accent" ? AppTheme.accentText
               : AppTheme.textPrimary)
            : _neutralSoft ? AppTheme.textPrimary
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
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
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
