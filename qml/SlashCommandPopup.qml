import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import MatrixClient

// Slash-command autocomplete, built like MentionPopup: never takes focus; the
// TextArea forwards Up/Down/Tab/Return/Escape. Model:
// MessageComposer.commandCompletions ([{name, argsHint, description,
// enabled}]). A disabled row is a permission hint (the server enforces),
// skipped by selection but still listed.
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
    // The composer drives open/close; auto-close would fight the editor's
    // focus.
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: completions ? completions.length : 0
    // Rows are two lines (command with argument hint, then description). The
    // list is a clipped, scrollable ListView whose height follows what fits.
    readonly property int rowH: AppTheme.scaled(52)
    readonly property int headerH: AppTheme.scaled(24)
    readonly property int visibleRows: Math.max(1, Math.min(count, 6))

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
        commandList.positionViewAtIndex(currentIndex, ListView.Contain)
    }
    function moveUp() {
        if (count > 0)
            currentIndex = (currentIndex - 1 + count) % count
        commandList.positionViewAtIndex(currentIndex, ListView.Contain)
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

        ListView {
            id: commandList
            objectName: "slashCommandPopupList"
            width: parent.width
            height: root.height - root.headerH - root.padding * 2
            // Nothing may be drawn outside the panel.
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.completions
            currentIndex: root.currentIndex
            // The list follows the keyboard; the popup never takes focus.
            interactive: contentHeight > height

            ScrollBar.vertical: AppScrollBar {
                thin: true
                objectName: "slashCommandPopupScrollBar"
                policy: commandList.contentHeight > commandList.height
                        ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }

            delegate: Rectangle {
                id: commandRow
                required property var modelData
                required property int index
                readonly property bool selected: index === root.currentIndex
                readonly property bool rowEnabled: modelData.enabled !== false
                width: ListView.view.width
                height: root.rowH
                radius: AppTheme.radiusSm
                color: selected ? AppTheme.stormSelection : "transparent"

                // The ground under a row's text is the selection chip over the
                // panel when selected, and the panel otherwise; grade inks
                // against the composite (stormSelection is translucent in some
                // palettes). textMuted passes on resting rows but fails on
                // selected ones in most palettes.
                readonly property color rowFill:
                    AppTheme.flatten(commandRow.color, AppTheme.stormPanel)

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
                        objectName: "slashCommandName"
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.scaled(13)
                        font.weight: Font.DemiBold
                        // Not bolt: outside Storm bolt is the accent and
                        // stormSelection is a tint of the same hue, so the
                        // selected name failed AA on ten of eleven palettes.
                        // stormText, as in MentionPopup, clears AA everywhere.
                        // Deriving bolt was rejected: it made the description
                        // as bright as the name (see legibleInkOn in
                        // AppTheme.qml).
                        color: rowEnabled
                               ? AppTheme.stormText
                               : AppTheme.legibleInkOn(AppTheme.textMuted,
                                                       commandRow.rowFill)
                    }
                    Label {
                        width: parent.width
                        textFormat: Text.PlainText
                        elide: Label.ElideRight
                        text: rowEnabled
                              ? (modelData.description || "")
                              : qsTr("%1 · you lack the required power level")
                                    .arg(modelData.description || "")
                        objectName: "slashCommandDescription"
                        font.pixelSize: AppTheme.fontChip
                        // Derived against the fill this row paints; where the
                        // token already clears it (every resting row) it is
                        // returned unchanged.
                        color: AppTheme.legibleInkOn(AppTheme.textMuted,
                                                     commandRow.rowFill)
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
