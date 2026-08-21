import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Settings → Appearance → Custom theme.
//
// Two columns: the editable roles on the left, a live fake room on the right.
// Editing a colour repaints the preview through the SAME path the real app
// uses — CustomThemeStore writes, Main.qml pushes into AppTheme, every
// consumer rebinds — so what the preview shows is what the app will look
// like, rather than a second rendering that can disagree with it.
//
// The dialog does NOT hold a draft. Every change is committed to
// CustomThemeStore immediately, which is why there is no Save button and why
// "Reset all" is the undo. A draft would need a second copy of the palette to
// live somewhere, and the two could then disagree about what the user picked.
AppDialog {
    id: root
    objectName: "themeEditorDialog"

    title: qsTr("Custom theme")
    standardButtons: Dialog.Close
    modal: true

    // Wide: the preview is only useful at something close to its natural
    // size, and the role list needs its labels unelided.
    implicitWidth: Math.min(940, Overlay.overlay ? Overlay.overlay.width - 80 : 940)

    readonly property var store: app.customTheme

    // Which role the colour dialog is currently editing. Held here rather
    // than on the row, because a Repeater delegate can be destroyed while a
    // modal file/colour dialog is open and the pending role would go with it.
    property string editingRole: ""

    ColorDialog {
        id: colorPicker
        // Opening on the role's CURRENT effective colour, not on white:
        // the user is adjusting a palette, not starting from nothing.
        onAccepted: {
            if (root.editingRole.length > 0) {
                root.store.setColor(root.editingRole,
                                    colorPicker.selectedColor.toString().toUpperCase())
            }
            root.editingRole = ""
        }
        onRejected: root.editingRole = ""
    }

    // The colour a role resolves to right now: the user's override if there is
    // one, otherwise whatever the base theme says. This is the swatch value
    // AND the colour the picker opens on.
    function effectiveColor(rolekey) {
        var overrides = root.store.colors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var pal = AppTheme.paletteForTheme(root.store.baseTheme)
        if (pal[rolekey] !== undefined)
            return pal[rolekey]
        // paletteForTheme exposes the preview subset, not every role. For
        // anything outside it, fall back to the LIVE token so the swatch is
        // still truthful rather than showing transparent black.
        return AppTheme[rolekey] !== undefined ? AppTheme[rolekey]
                                               : AppTheme.stormTextMuted
    }

    function isOverridden(rolekey) {
        var overrides = root.store.colors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    // Roles grouped in declaration order. CustomThemeStore owns the order and
    // the group names (they are translated there), so this only has to notice
    // where the group changes.
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

        // ── Left: the editable roles ─────────────────────────────────────
        ColumnLayout {
            Layout.preferredWidth: 360
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            // Base theme. Every role the user has NOT overridden resolves
            // through this, so changing it restyles everything untouched.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing4
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
                    // The custom theme itself is not a valid base: it would
                    // be a cycle, and QML resolves that as an undefined
                    // palette rather than as an error.
                    model: AppTheme.themeList.filter((t) => t.id !== 12)
                    textRole: "name"
                    valueRole: "id"
                    // indexOfValue() is -1 at creation time, so the index is
                    // synced explicitly rather than bound.
                    function syncIndex() {
                        var i = indexOfValue(root.store.baseTheme)
                        if (i >= 0 && i !== currentIndex)
                            currentIndex = i
                    }
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
                          : qsTr("%n colour(s) overridden. Everything else follows the theme above.",
                                 "custom theme, count of edited roles",
                                 root.store.overrideCount)
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }

            // The role list. Scrolls: there are more roles than fit, and the
            // preview must stay visible while scrolling them.
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 260
                clip: true
                ScrollBar.vertical: AppScrollBar {}

                ColumnLayout {
                    width: parent.width
                    spacing: AppTheme.spacing8

                    Repeater {
                        model: root.roleGroups
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing4

                            Label {
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
                                    Layout.fillWidth: true
                                    implicitHeight: roleLayout.implicitHeight
                                                    + AppTheme.spacing6 * 2
                                    radius: AppTheme.radiusSm
                                    color: roleHover.hovered
                                           ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                           : "transparent"

                                    HoverHandler { id: roleHover }

                                    RowLayout {
                                        id: roleLayout
                                        anchors.fill: parent
                                        anchors.leftMargin: AppTheme.spacing6
                                        anchors.rightMargin: AppTheme.spacing6
                                        anchors.topMargin: AppTheme.spacing6
                                        anchors.bottomMargin: AppTheme.spacing6
                                        spacing: AppTheme.spacing8

                                        // The swatch IS the button. A colour
                                        // control that needs a separate
                                        // "Edit" affordance reads as a label.
                                        Rectangle {
                                            objectName: "themeSwatch_" + roleRow.modelData.key
                                            implicitWidth: 28
                                            implicitHeight: 28
                                            radius: AppTheme.radiusSm
                                            color: root.effectiveColor(roleRow.modelData.key)
                                            border.width: root.isOverridden(roleRow.modelData.key)
                                                          ? 2 : 1
                                            border.color: root.isOverridden(roleRow.modelData.key)
                                                          ? AppTheme.bolt
                                                          : AppTheme.stormBorder
                                            Accessible.role: Accessible.Button
                                            Accessible.name: qsTr("Change %1")
                                                .arg(roleRow.modelData.label)
                                            TapHandler {
                                                onTapped: {
                                                    root.editingRole = roleRow.modelData.key
                                                    colorPicker.selectedColor =
                                                        root.effectiveColor(roleRow.modelData.key)
                                                    colorPicker.open()
                                                }
                                            }
                                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Label {
                                                text: roleRow.modelData.label
                                                color: AppTheme.stormText
                                                font.pixelSize: AppTheme.textMeta
                                                font.weight: AppTheme.weightStrong
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Label {
                                                text: roleRow.modelData.hint
                                                color: AppTheme.stormTextMuted
                                                font.pixelSize: AppTheme.textMicro
                                                wrapMode: Text.WordWrap
                                                Layout.fillWidth: true
                                            }
                                        }

                                        // Only offered where there is something
                                        // to revert — a reset button on an
                                        // untouched role does nothing and says
                                        // nothing.
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
        }

        // ── Right: the live preview ──────────────────────────────────────
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
                Layout.fillWidth: true
                Layout.preferredHeight: 320
                radius: AppTheme.radiusMd
                color: AppTheme.stormInset
                border.width: 1
                border.color: AppTheme.stormBorder
                clip: true

                // The preview renders the LIVE theme, so it is only truthful
                // while the custom theme is the selected one. Selecting it is
                // therefore part of opening the editor (see SettingsScreen),
                // and this notice covers the case where the user switches
                // theme in another pane while the dialog is open.
                ThemePreviewDemo {
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: app.settings.theme === 12
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

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                AppButton {
                    objectName: "themeResetAllButton"
                    text: qsTr("Reset all colours")
                    enabled: root.store.overrideCount > 0
                    onClicked: resetAllConfirm.open()
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    // Discarding a palette someone has spent time on is worth one question.
    AppDialog {
        id: resetAllConfirm
        title: qsTr("Reset all colours?")
        standardButtons: Dialog.Yes | Dialog.Cancel
        destructive: true
        onAccepted: root.store.resetAll()
        Label {
            width: parent ? parent.width : 320
            wrapMode: Text.WordWrap
            text: qsTr("Every colour goes back to the theme you started from. The theme itself is kept.")
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
        }
    }
}
