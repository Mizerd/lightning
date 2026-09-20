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
    // A row is two lines — the command with its argument hint, then the
    // description — so 40px was never enough: the Column drew every one of
    // the ~15 commands at its natural height and the popup's own height
    // stopped at eight, so the tail spilled out BELOW the panel, unclipped
    // and unreachable. The list is a real ListView now: clipped, scrollable,
    // and the height follows what fits.
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
            // The whole point: nothing may be drawn outside the panel.
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.completions
            currentIndex: root.currentIndex
            // The list follows the keyboard, and the keyboard follows the
            // editor — the popup itself never takes focus.
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

                // ── A ROW IS TWO SURFACES, AND ONLY ONE WAS EVER MEASURED ──
                //
                // The pixels under this row's text are the selection chip
                // OVER the panel when the row is selected and the panel
                // itself when it is not, so an ink graded on `stormPanel`
                // answers for one of the two. Measured 2026-09-20 across all
                // eleven palettes, the description inked `textMuted` clears
                // 4.5:1 AA on every RESTING row (floor 4.59, Deep Teal) and
                // fails on EIGHT selected ones — Graphite 3.09, Indigo Night
                // 3.44, Deep Teal 3.45, Nordic 3.46, Midnight 3.58,
                // Lightning Dark 3.61, Purple Dusk 3.72, Storm 4.38 — which
                // is exactly why measuring the resting state found nothing.
                //
                // This is `flatten`'s reason for existing: `stormSelection`
                // is TRANSLUCENT in some palettes (Storm's row fills are
                // alpha'd), so the ground is the composite, never the chip's
                // own colour property.
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
                        // ── THE SELECTED COMMAND'S NAME IS NOT BOLT ───────
                        //
                        // It was, and `bolt` is the one token that cannot
                        // carry it: on the ten non-Storm palettes `bolt`
                        // routes to `accent` while `stormSelection` routes to
                        // `hover`, which is a lighter tint of the SAME hue
                        // family — two mid tones, one on the other. Measured
                        // 2026-09-20 on the fill this row paints, the name of
                        // the command you are about to run failed AA on ten
                        // of eleven: Indigo Night 1.61, Lightning Dark 1.65,
                        // Midnight 1.69, Nordic 1.83, Graphite 1.87, Purple
                        // Dusk 2.21, Deep Teal 4.00, Lightning Light 4.14,
                        // Moss Light 4.27, Warm 4.46. Only Storm passed
                        // (7.53), because Storm is the one palette where
                        // `bolt` is the actual bolt — which is precisely why
                        // it looked right to whoever wrote it.
                        //
                        // `stormText` is what MentionPopup — "same
                        // construction", per this file's own header — already
                        // does: it inks the selected row's NAME with
                        // stormText and spends bolt only on the matched
                        // SUBSTRING, the yellow discipline. Here the whole
                        // string is the match, so bolt-on-everything was the
                        // defect. 6.40-13.17:1 on all eleven, and the row is
                        // still marked by its fill and its Return keycap.
                        //
                        // DERIVING bolt was measured and REJECTED: it reaches
                        // AA but lands the name at 4.52-4.92 beside a
                        // description derived to 4.55-4.88, a hierarchy ratio
                        // of 0.91-1.05 — on four palettes the description
                        // would be BRIGHTER than the command name. That is
                        // the collapse recorded as refuted beside
                        // `legibleInkOn` in AppTheme.qml. stormText gives
                        // 1.38-2.70.
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
                        // Derived against the fill THIS ROW paints, never
                        // against the panel and never against the worst of
                        // the two: where the token already clears its own
                        // fill — every resting row, on every palette —
                        // `legibleInkOn` hands it straight back, so the
                        // resting state is bit-identical to before and the
                        // name/description hierarchy is untouched there.
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
