import QtQuick
import MatrixClient

// v0.6.5: the small status pill shared by the redesigned surfaces —
// Verified (1p), ACTIVE (1h), MOD (1q), LOUD (1q), unread counts (1h).
// tone picks the semantic colour family; solid switches from the soft
// 10%-tint treatment (with a 25% border) to a filled pill.
//
// Storm skin (SPEC-storm-language §3.7): `storm: true` renders the mono
// storm chip vocabulary — "bolt" tone is THE yellow chip (ACTIVE / MOD /
// VERIFIED: bolt fill, panel ink; also unread badges, replacing red);
// success/danger/neutral remap to the storm palette. Themed hosts (room
// list) keep the default treatment.
Rectangle {
    id: root

    property string label: ""
    // Optional leading Material Symbols glyph (verified_user on 1p's chip).
    property string iconName: ""
    // neutral | accent | success | danger | onAccent | bolt (storm-only).
    // onAccent is the chip ON an accent-gradient fill (the ACTIVE card): it
    // inks in accentText so bright-accent themes with dark ink stay correct.
    property string tone: "neutral"
    property bool solid: false
    property bool storm: false
    property int textSize: AppTheme.fontChip

    readonly property bool _boltChip: storm && (tone === "bolt"
                                                || tone === "accent"
                                                || tone === "onAccent")
    readonly property color _base: {
        if (storm) {
            if (_boltChip) return AppTheme.bolt
            if (tone === "success") return AppTheme.stormSuccess
            if (tone === "danger") return AppTheme.stormDanger
            return AppTheme.stormTextMuted
        }
        return tone === "accent" ? AppTheme.accent
             : tone === "success" ? AppTheme.presenceOnline
             : tone === "danger" ? AppTheme.mentionBadge
             : tone === "onAccent" ? AppTheme.accentText
             : AppTheme.textMuted
    }
    readonly property color _ink: {
        if (storm)
            return _boltChip ? AppTheme.stormPanel
                 : solid ? AppTheme.stormPanel
                 : _base
        return solid
            ? (tone === "danger" ? AppTheme.dangerText
               : tone === "accent" ? AppTheme.accentText
               : AppTheme.textPrimary)
            : _base
    }

    implicitWidth: chipContent.implicitWidth + 2 * AppTheme.spacing6
    implicitHeight: chipContent.implicitHeight + 2 * AppTheme.spacing2
    radius: AppTheme.radiusPill
    color: {
        if (storm)
            return _boltChip ? AppTheme.bolt
                 : solid ? _base
                 : Qt.alpha(_base, 0.10)
        return solid ? _base
                     : Qt.alpha(_base, tone === "onAccent" ? 0.25 : 0.10)
    }
    border.width: _boltChip || solid || (!storm && tone === "onAccent") ? 0 : 1
    border.color: Qt.alpha(_base, storm ? 0.30 : 0.25)

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
            font.family: root.storm ? AppTheme.monoFont : AppTheme.uiFont
            font.pixelSize: root.textSize
            font.weight: Font.Bold
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
