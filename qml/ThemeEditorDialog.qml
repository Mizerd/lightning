import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Appearance → Custom theme.
//
// Three columns: the editable roles, a live preview of the whole app, and the
// colour picker for whichever role is being edited. The picker is INLINE
// rather than a platform ColorDialog because a modal dialog opens on top of
// the preview, and watching the preview is the entire point of the editor.
//
// The dialog takes nearly the whole window on purpose. It is not a settings
// popover; it is a workspace, and the preview is only useful at a size where
// a person can actually read the room list and the messages in it.
//
// It holds no draft. Every change commits to CustomThemeStore immediately,
// which is why there is no Save button and why Reset is the undo — a draft
// would need a second copy of the palette, and the two could then disagree
// about what the user picked.
AppDialog {
    id: root
    objectName: "themeEditorDialog"

    title: qsTr("Custom theme")
    standardButtons: Dialog.Close
    modal: true

    readonly property real _availW: Overlay.overlay ? Overlay.overlay.width : 1280
    readonly property real _availH: Overlay.overlay ? Overlay.overlay.height : 800
    implicitWidth: Math.max(760, _availW - 64)
    implicitHeight: Math.max(520, _availH - 64)

    readonly property var store: app.customTheme

    // The role currently open in the picker. Held on the dialog, not on the
    // row: a Repeater delegate can be destroyed while the picker is open (the
    // list scrolls, the group filter changes) and the pending role would go
    // with it.
    property string editingRole: ""
    property string editingLabel: ""

    function effectiveColor(rolekey) {
        var overrides = root.store.colors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var pal = AppTheme.paletteForTheme(root.store.baseTheme)
        if (pal[rolekey] !== undefined)
            return pal[rolekey]
        // paletteForTheme exposes the preview subset, not every role, so
        // anything outside it falls back to the LIVE token — still truthful,
        // where transparent black would look like a broken theme.
        return AppTheme[rolekey] !== undefined ? AppTheme[rolekey]
                                               : AppTheme.stormTextMuted
    }

    function isOverridden(rolekey) {
        var overrides = root.store.colors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    function beginEdit(key, label) {
        root.editingRole = key
        root.editingLabel = label
        picker.load(root.effectiveColor(key))
    }

    readonly property var roleGroups: {
        var out = []
        var seen = {}
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            var g = list[i].group
            if (seen[g] === undefined) {
                seen[g] = out.length
                out.push({ name: g, items: [] })
            }
            out[seen[g]].items.push(list[i])
        }
        return out
    }

    RowLayout {
        spacing: AppTheme.spacing16

        // ── Roles ────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.preferredWidth: 320
            Layout.minimumWidth: 320
            Layout.maximumWidth: 320
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            Label {
                text: qsTr("Start from")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }
            AppComboBox {
                id: baseCombo
                objectName: "customThemeBaseCombo"
                storm: true
                Layout.fillWidth: true
                // 12 is this theme itself: a cycle QML resolves as an
                // undefined palette rather than as an error.
                model: AppTheme.themeList.filter((t) => t.id !== 12)
                textRole: "name"
                valueRole: "id"
                function syncIndex() { syncToValue(root.store.baseTheme) }
                Component.onCompleted: syncIndex()
                onActivated: root.store.baseTheme = currentValue
                Connections {
                    target: root.store
                    function onCustomThemeChanged() {
                        Qt.callLater(baseCombo.syncIndex)
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.store.overrideCount === 0
                      ? qsTr("Nothing is overridden yet — this theme is identical to its starting point.")
                      : qsTr("%n colour(s) changed. Everything else follows the theme above.",
                             "custom theme, count of edited roles",
                             root.store.overrideCount)
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }

            ScrollView {
                id: roleScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical: AppScrollBar {}

                ColumnLayout {
                    // Bound to availableWidth, NOT to `parent.width`: inside a
                    // ScrollView the parent is the Flickable's contentItem,
                    // whose width is 0 until it has laid out. Children then get
                    // zero width and every wrapping Label breaks one word per
                    // line, which is exactly how this shipped the first time.
                    width: roleScroll.availableWidth
                    spacing: AppTheme.spacing8

                    Repeater {
                        model: root.roleGroups
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.topMargin: AppTheme.spacing6
                                text: modelData.name
                                color: AppTheme.sectionLabelColor
                                font.family: AppTheme.menuSectionFont
                                font.pixelSize: AppTheme.menuSectionSize
                                font.weight: AppTheme.menuSectionWeight
                            }

                            Repeater {
                                model: modelData.items
                                delegate: Rectangle {
                                    id: roleRow
                                    required property var modelData
                                    objectName: "themeRole_" + modelData.key
                                    readonly property bool editing:
                                        root.editingRole === modelData.key
                                    Layout.fillWidth: true
                                    implicitHeight: 40
                                    radius: AppTheme.radiusSm
                                    color: editing ? AppTheme.stormSelection
                                         : roleHover.hovered
                                           ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                           : "transparent"

                                    HoverHandler {
                                        id: roleHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: root.beginEdit(roleRow.modelData.key,
                                                                 roleRow.modelData.label)
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: AppTheme.spacing6
                                        anchors.rightMargin: AppTheme.spacing6
                                        spacing: AppTheme.spacing8

                                        Rectangle {
                                            objectName: "themeSwatch_" + roleRow.modelData.key
                                            implicitWidth: 26
                                            implicitHeight: 26
                                            radius: AppTheme.radiusSm
                                            color: root.effectiveColor(roleRow.modelData.key)
                                            // A changed role is marked on the
                                            // swatch, so the list reads as a
                                            // diff at a glance.
                                            border.width: root.isOverridden(roleRow.modelData.key)
                                                          ? 2 : 1
                                            border.color: root.isOverridden(roleRow.modelData.key)
                                                          ? AppTheme.bolt
                                                          : AppTheme.stormBorder
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: roleRow.modelData.label
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.textMeta
                                            elide: Label.ElideRight
                                        }

                                        IconButton {
                                            objectName: "themeRoleReset_" + roleRow.modelData.key
                                            visible: root.isOverridden(roleRow.modelData.key)
                                            iconName: "undo"
                                            size: "sm"
                                            storm: true
                                            Accessible.name: qsTr("Reset %1 to the base theme")
                                                .arg(roleRow.modelData.label)
                                            onClicked: root.store.resetColor(roleRow.modelData.key)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            AppButton {
                objectName: "themeResetAllButton"
                Layout.fillWidth: true
                size: "sm"
                storm: true
                text: qsTr("Reset all colours")
                enabled: root.store.overrideCount > 0
                onClicked: resetAllConfirm.open()
            }
        }

        // ── Preview ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            Label {
                text: qsTr("Preview")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }

            Rectangle {
                id: previewFrame
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: AppTheme.radiusMd
                color: AppTheme.stormInset
                border.width: 1
                border.color: AppTheme.stormBorder
                clip: true

                // The preview renders at its NATURAL size and is scaled to
                // fit, rather than being stretched by a Layout. Stretching a
                // shell mock re-flows it into proportions the real window
                // never has — a 1200px-wide room list, a two-line composer —
                // and then the user is judging colours on a layout that does
                // not exist. Scaling keeps it a faithful miniature.
                ThemePreviewDemo {
                    id: preview
                    visible: app.settings.theme === 12
                    width: implicitWidth
                    height: implicitHeight
                    transformOrigin: Item.TopLeft
                    scale: Math.max(0.5, Math.min(
                        (previewFrame.width - 24) / implicitWidth,
                        (previewFrame.height - 24) / implicitHeight))
                    x: (previewFrame.width - implicitWidth * scale) / 2
                    y: (previewFrame.height - implicitHeight * scale) / 2
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - AppTheme.spacing24
                    visible: app.settings.theme !== 12
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Select the custom theme to preview it.")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textBody
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("This is a sample conversation, not one of your rooms.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        // ── Picker ───────────────────────────────────────────────────────
        // Occupies the column whether or not a role is open, so choosing a
        // role does not re-flow the preview next to it.
        ColumnLayout {
            Layout.preferredWidth: 268
            Layout.minimumWidth: 268
            Layout.maximumWidth: 268
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            ColorPickerPanel {
                id: picker
                objectName: "themeColorPicker"
                Layout.fillWidth: true
                visible: root.editingRole.length > 0
                title: root.editingLabel
                canReset: root.editingRole.length > 0
                          && root.isOverridden(root.editingRole)
                onPicked: (value) => {
                    if (root.editingRole.length > 0)
                        root.store.setColor(root.editingRole, root.toHex(value))
                }
                onResetRequested: {
                    if (root.editingRole.length > 0) {
                        root.store.resetColor(root.editingRole)
                        picker.load(root.effectiveColor(root.editingRole))
                    }
                }
                onClosed: root.editingRole = ""
            }

            Label {
                Layout.fillWidth: true
                visible: root.editingRole.length === 0
                wrapMode: Text.WordWrap
                text: qsTr("Pick a part of the window on the left to give it a colour.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }

            // The hint for the role being edited, under the picker where
            // there is room for a full sentence — the list rows elide.
            Label {
                Layout.fillWidth: true
                visible: root.editingRole.length > 0
                wrapMode: Text.WordWrap
                text: {
                    var list = root.store.roles
                    for (var i = 0; i < list.length; ++i) {
                        if (list[i].key === root.editingRole)
                            return list[i].hint
                    }
                    return ""
                }
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }

            Item { Layout.fillHeight: true }
        }
    }

    // CustomThemeStore stores #RRGGBB and nothing else; the picker hands back
    // a QML color, whose toString() is #AARRGGBB.
    function toHex(c) {
        function two(v) {
            var s = Math.round(v * 255).toString(16).toUpperCase()
            return s.length < 2 ? "0" + s : s
        }
        return "#" + two(c.r) + two(c.g) + two(c.b)
    }

    AppDialog {
        id: resetAllConfirm
        title: qsTr("Reset all colours?")
        standardButtons: Dialog.Yes | Dialog.Cancel
        destructive: true
        onAccepted: {
            root.store.resetAll()
            if (root.editingRole.length > 0)
                picker.load(root.effectiveColor(root.editingRole))
        }
        Label {
            width: parent ? parent.width : 320
            wrapMode: Text.WordWrap
            text: qsTr("Every colour goes back to the theme you started from. The theme itself is kept.")
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
        }
    }
}
