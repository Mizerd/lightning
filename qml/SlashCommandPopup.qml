import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import MatrixClient

// v0.9 slash commands: the composer's command autocomplete popup. Same
// construction as MentionPopup — a flat Lightning surface floating above the
// composer input that deliberately never takes focus; the TextArea keeps the
// caret and forwards Up/Down/Tab/Return/Escape. The model is
// MessageComposer.commandCompletions ([{name, argsHint, description,
// enabled}]); a disabled row is a permission COURTESY hint (the server is
// the enforcer) and is skipped by selection but still listed, so the user
// learns the command exists.
Popup {
    id: root
    objectName: "slashCommandPopup"

    property var completions: []
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(string name)

    parent: Overlay.overlay
    focus: false
    // The composer drives open/close; auto-close (focus/press-outside)
    // would fight the editor keeping focus.
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: completions ? completions.length : 0
    readonly property int rowH: AppTheme.scaled(40)
    readonly property int headerH: AppTheme.scaled(24)
    readonly property int visibleRows: Math.max(1, Math.min(count, 8))

    width: Math.max(280, Math.min(anchorWidth, 420))
    height: headerH + visibleRows * rowH + padding * 2
    x: parent ? Math.max(AppTheme.spacing4,
                         Math.min(anchorInputTop.x,
                                  parent.width - width - AppTheme.spacing4))
              : anchorInputTop.x
    y: Math.max(AppTheme.spacing4,
                anchorInputTop.y - height - AppTheme.spacing4)

    onCountChanged: {
        if (currentIndex >= count)
            currentIndex = Math.max(0, count - 1)
        if (currentIndex < 0)
            currentIndex = 0
        if (visible && count === 0)
            close()
    }
    onOpened: currentIndex = 0

    function moveDown() {
        if (count > 0)
            currentIndex = (currentIndex + 1) % count
    }
    function moveUp() {
        if (count > 0)
            currentIndex = (currentIndex - 1 + count) % count
    }
    function accept() {
        if (count === 0)
            return
        var entry = completions[currentIndex]
        if (!entry || entry.enabled === false)
            return
        root.chosen(entry.name)
    }

    background: Item {
        MultiEffect {
            source: commandSurface
            anchors.fill: commandSurface
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadowSoft
            shadowBlur: 0.9
            shadowVerticalOffset: AppTheme.elevationPopoverY
            shadowHorizontalOffset: 0
        }
        Rectangle {
            id: commandSurface
            objectName: "slashCommandPopupSurface"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.menuRadius
        }
    }

    contentItem: Column {
        spacing: 0

        Label {
            objectName: "slashCommandPopupHeader"
            width: parent.width
            height: root.headerH
            verticalAlignment: Text.AlignVCenter
            textFormat: Text.PlainText
            elide: Label.ElideRight
            text: qsTr("Commands")
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontChip
            font.weight: Font.DemiBold
            font.letterSpacing: AppTheme.trackingStorm
            font.capitalization: Font.AllUppercase
            color: AppTheme.stormTextFaint
        }

        Repeater {
            model: root.completions
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool selected: index === root.currentIndex
                readonly property bool rowEnabled: modelData.enabled !== false
                width: parent.width
                height: root.rowH
                radius: AppTheme.radiusSm
                color: selected ? AppTheme.stormSelection : "transparent"

                Accessible.role: Accessible.ListItem
                Accessible.name: "/" + modelData.name + " "
                                 + (modelData.description || "")

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: keycap.left
                    anchors.leftMargin: AppTheme.spacingXS
                    anchors.rightMargin: AppTheme.spacingXS
                    spacing: 0

                    Label {
                        width: parent.width
                        textFormat: Text.PlainText
                        elide: Label.ElideRight
                        text: "/" + modelData.name
                              + (modelData.argsHint && modelData.argsHint.length > 0
                                     ? " " + modelData.argsHint : "")
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.scaled(13)
                        font.weight: Font.DemiBold
                        color: !rowEnabled ? AppTheme.textMuted
                               : selected ? AppTheme.bolt : AppTheme.text
                    }
                    Label {
                        width: parent.width
                        textFormat: Text.PlainText
                        elide: Label.ElideRight
                        text: rowEnabled
                              ? (modelData.description || "")
                              : qsTr("%1 · you lack the required power level")
                                    .arg(modelData.description || "")
                        font.pixelSize: AppTheme.fontChip
                        color: AppTheme.textMuted
                    }
                }
                MenuKeycap {
                    id: keycap
                    anchors.right: parent.right
                    anchors.rightMargin: AppTheme.spacingXS
                    anchors.verticalCenter: parent.verticalCenter
                    visible: selected && rowEnabled
                    iconName: "keyboard_return"
                    active: true
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: rowEnabled
                    onClicked: {
                        root.currentIndex = index
                        root.accept()
                    }
                }
            }
        }
    }
}
