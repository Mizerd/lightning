import QtQuick
import QtQuick.Controls
import MatrixClient

// A labelled row of image packs: avatar plus name, one chip per pack that
// holds images of `usage`. Used by the sticker picker and the emoji picker's
// custom-emoji section.
ListView {
    id: strip

    property var stickers: app.stickers
    property string usage: "sticker"
    property string currentPackId: ""
    property bool showAll: false
    property bool allCurrent: false
    signal packChosen(string packId)

    readonly property int packCount: {
        var _rev = strip.stickers ? strip.stickers.revision : 0
        var packs = strip.stickers ? strip.stickers.packs : null
        if (!packs)
            return 0
        var n = 0
        for (var i = 0; i < packs.count; ++i) {
            var p = packs.get(i)
            var images = strip.usage === "emoticon" ? p.emoticonCount
                                                    : p.stickerCount
            if (images > 0 && !p.hidden)
                ++n
        }
        return n
    }

    readonly property real chipHeight: 30
    readonly property real chipMaxWidth: 148

    implicitHeight: chipHeight + 4
    orientation: ListView.Horizontal
    clip: true
    spacing: AppTheme.spacingXS
    boundsBehavior: Flickable.StopAtBounds
    model: strip.stickers ? strip.stickers.packs : null
    SmoothWheelArea { axis: "horizontal" }

    header: Item {
        visible: strip.showAll
        width: visible ? allChip.width + strip.spacing : 0
        height: strip.height

        AbstractButton {
            id: allChip
            objectName: "packChipAll"
            anchors.verticalCenter: parent.verticalCenter
            width: allLabel.implicitWidth + AppTheme.spacing12 * 2
            height: strip.chipHeight
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.RadioButton
            Accessible.name: allLabel.text
            Accessible.checked: strip.allCurrent
            background: Rectangle {
                radius: height / 2
                color: strip.allCurrent ? AppTheme.stormSelection
                     : (allChip.down || allChip.hovered) ? AppTheme.hover
                     : "transparent"
                border.width: allChip.visualFocus || strip.allCurrent ? 2 : 1
                border.color: allChip.visualFocus || strip.allCurrent
                              ? AppTheme.bolt : AppTheme.stormBorder
            }
            contentItem: Label {
                id: allLabel
                text: qsTr("All")
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: strip.allCurrent ? AppTheme.stormText
                                        : AppTheme.stormTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightMedium
            }
            onClicked: strip.packChosen("")
        }
    }

    delegate: AbstractButton {
        id: chip
        objectName: "packChip"
        required property int index
        required property string packId
        required property string displayName
        required property string avatarUrl
        required property int stickerCount
        required property int emoticonCount
        required property bool hidden

        readonly property int imageCount:
            strip.usage === "emoticon" ? emoticonCount : stickerCount
        readonly property bool current:
            !strip.allCurrent && chip.packId === strip.currentPackId
        readonly property string label:
            chip.displayName.length > 0 ? chip.displayName : qsTr("Untitled pack")

        visible: imageCount > 0 && !hidden
        width: visible
               ? Math.min(strip.chipMaxWidth,
                          AppTheme.spacing6 + 20 + AppTheme.spacing6
                          + Math.ceil(nameMetrics.advanceWidth)
                          + AppTheme.spacing12)
               : 0
        height: strip.chipHeight
        y: (strip.height - height) / 2
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.RadioButton
        Accessible.name: chip.label
        Accessible.checked: chip.current
        ToolTip.text: chip.label
        ToolTip.visible: hovered && chipLabel.truncated
        ToolTip.delay: 500

        background: Rectangle {
            radius: height / 2
            color: chip.current ? AppTheme.stormSelection
                 : (chip.down || chip.hovered) ? AppTheme.hover
                 : "transparent"
            border.width: chip.visualFocus || chip.current ? 2 : 1
            border.color: chip.visualFocus || chip.current
                          ? AppTheme.bolt : AppTheme.stormBorder
        }
        contentItem: Item {
            Avatar {
                id: chipAvatar
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacing6
                anchors.verticalCenter: parent.verticalCenter
                size: 20
                circle: true
                mxc: chip.avatarUrl
                name: chip.label
                colorKey: chip.packId
            }
            Label {
                id: chipLabel
                objectName: "packChipLabel"
                // Pack names are room state: never markup.
                textFormat: Text.PlainText
                anchors.left: chipAvatar.right
                anchors.leftMargin: AppTheme.spacing6
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing12
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                elide: Text.ElideRight
                color: chip.current ? AppTheme.stormText
                                    : AppTheme.stormTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightMedium
            }
        }
        onClicked: strip.packChosen(chip.packId)

        // Measured apart from chipLabel, whose width follows the chip's.
        TextMetrics {
            id: nameMetrics
            font: chipLabel.font
            text: chip.label
        }
    }
}
