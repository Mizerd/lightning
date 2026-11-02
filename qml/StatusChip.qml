import QtQuick
import MatrixClient

Rectangle {
    id: root

    property string label: ""
    property string iconName: ""
    property string tone: "neutral"
    property bool solid: false
    property bool storm: false
    property int textSize: AppTheme.textMicro

    readonly property bool _boltChip: storm && (tone === "bolt"
                                                || tone === "onAccent")
    readonly property color _base: {
        if (storm) {
            if (_boltChip) return AppTheme.bolt
            if (tone === "accent") return AppTheme.stormLink
            if (tone === "success") return AppTheme.stormChipSuccess
            if (tone === "warning") return AppTheme.chipWarning
            if (tone === "danger") return AppTheme.stormChipDanger
            if (tone === "info") return AppTheme.info
            return AppTheme.stormTextMuted
        }
        return tone === "accent" ? AppTheme.accent
             : tone === "success" ? AppTheme.chipSuccess
             : tone === "warning" ? AppTheme.chipWarning
             : tone === "danger" ? AppTheme.chipDanger
             : tone === "info" ? AppTheme.info
             : tone === "onAccent" ? AppTheme.accentText
             : AppTheme.textMuted
    }
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
    Accessible.role: Accessible.StaticText
    Accessible.name: root.label
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
            textFormat: Text.PlainText
            objectName: "chipLabel"
            visible: root.label.length > 0
            text: root.label
            font.family: AppTheme.uiFont
            font.pixelSize: root.textSize
            font.weight: AppTheme.weightBold
            color: root._ink
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
