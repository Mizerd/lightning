import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Appearance → Custom theme.
//
// A FULL-WINDOW workspace, not a settings popover. Three columns: the editable
// roles, a live preview of the whole app, and the colour picker for whichever
// role is being edited. The picker is INLINE rather than a platform
// ColorDialog because a modal dialog opens on top of the preview, and watching
// the preview is the entire point of the editor.
//
// It is a Popup parented to `Overlay.overlay` and sized to it. It used to be an
// AppDialog, and AppDialog centres itself in its PARENT — which for a dialog
// declared inside the Appearance page is a scrolled settings column, not the
// window. It sized itself off `Overlay.overlay` too, whose fallback branch left
// it locked at 1216x736 and hanging off the bottom of the screen with the reset
// controls below the edge. Explicit geometry against the overlay removes both.
//
// The chrome is painted in AppTheme's INVARIANT editor tokens and draws its own
// buttons, fields and scrollbar. See the comment beside `editorCanvas` in
// AppTheme.qml: everything in the shared control set follows the storm*
// namespace, which follows the selected theme — so an editor built from it goes
// blank the moment someone paints their panel and their body ink the same
// colour, taking the reset button with it.
//
// It holds no draft. Every change commits to CustomThemeStore immediately,
// which is why there is no Save button and why Reset is the undo — a draft
// would need a second copy of the palette, and the two could then disagree
// about what the user picked.
Popup {
    id: root
    objectName: "themeEditorDialog"

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    modal: true
    closePolicy: Popup.CloseOnEscape

    // Opening the editor with nothing to edit would show an empty name field
    // and no chips. A theme with no overrides is a real, harmless state — it
    // simply follows its base — so the first one is created here rather than
    // waiting for the first colour pick.
    onOpened: if (!root.store.exists) root.store.createTheme("")

    readonly property var store: app.customTheme

    // The role currently open in the picker. Held on the dialog, not on the
    // row: a Repeater delegate can be destroyed while the picker is open (the
    // list scrolls, the group filter changes) and the pending role would go
    // with it.
    property string editingRole: ""
    property string editingLabel: ""
    property bool confirmingReset: false
    // Import/share state. `notice` is a transient confirmation line; it is
    // cleared by the timer below so it cannot sit there claiming something
    // that happened a minute ago.
    property bool importing: false
    property string importError: ""
    property string notice: ""

    onNoticeChanged: if (notice.length > 0) noticeTimer.restart()
    Timer {
        id: noticeTimer
        interval: 4000
        onTriggered: root.notice = ""
    }

    function applyImport() {
        var payload = importField.text
        if (payload.trim().length === 0)
            return
        var reason = root.store.importTheme(payload)
        if (reason.length > 0) {
            root.importError = reason
            return
        }
        importField.text = ""
        root.importError = ""
        root.importing = false
        root.editingRole = ""
        root.notice = qsTr("Theme imported.")
    }

    // The clipboard shuttle for Share. A hidden TextEdit is how every other
    // copy in this application reaches the clipboard.
    TextEdit {
        id: themeClipboard
        visible: false
        width: 0
        height: 0
    }

    // The palette the preview paints. Resolved BY ID, so the preview shows the
    // custom theme whether or not the application is currently running it.
    // Main.qml keeps AppTheme.customOverrides / customBase bound to the
    // store, so id 12 already resolves to base-plus-overrides. Reading it
    // through paletteForTheme keeps the preview on the SAME resolver the
    // running application uses — a second merge here could disagree with it.
    readonly property var previewPalette: AppTheme.paletteForTheme(12)

    function effectiveColor(rolekey) {
        var overrides = root.store.colors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var pal = AppTheme.paletteForTheme(root.store.baseTheme)
        // paletteForTheme resolves SEMANTIC role names; a few store keys are
        // the palette's own spelling (inputBg -> inputBackground, mention ->
        // mentionBadge, reaction -> reactionBackground).
        var alias = root.storeKeyAliases[rolekey]
        var lookup = alias !== undefined ? alias : rolekey
        if (pal[lookup] !== undefined)
            return pal[lookup]
        return AppTheme.editorTextMuted
    }

    readonly property var storeKeyAliases: ({
        "inputBg": "inputBackground",
        "mention": "mentionBadge",
        "reaction": "reactionBackground"
    })

    function isOverridden(rolekey) {
        var overrides = root.store.colors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    function beginEdit(key, label) {
        root.editingRole = key
        root.editingLabel = label
        picker.load(root.effectiveColor(key))
    }

    function labelForRole(key) {
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return list[i].label
        }
        return key
    }

    function hintForRole(key) {
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return list[i].hint
        }
        return ""
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

    // ── Self-contained controls ──────────────────────────────────────────
    // Painted in the invariant editor tokens; see the file header.
    component EditorButton: AbstractButton {
        id: btn
        property bool primary: false
        property bool danger: false
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        implicitHeight: 34
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing20 * 2
        Accessible.role: Accessible.Button
        Accessible.name: text

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: btn.primary
                   ? (btn.down || btn.hovered ? Qt.lighter(AppTheme.editorAccent, 1.08)
                                              : AppTheme.editorAccent)
                   : btn.down ? AppTheme.editorSelection
                   : btn.hovered ? AppTheme.editorInset : "transparent"
            border.width: btn.primary ? 0 : 1
            border.color: btn.danger ? AppTheme.editorDanger
                                     : AppTheme.editorBorderStrong
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                visible: btn.visualFocus
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: AppTheme.editorAccent
            }
        }
        contentItem: Label {
            id: btnLabel
            text: btn.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: btn.primary ? AppTheme.editorAccentInk
                 : btn.danger ? AppTheme.editorDanger
                              : AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
            opacity: btn.enabled ? 1.0 : 0.45
        }
    }

    background: Rectangle {
        color: AppTheme.editorCanvas
    }

    contentItem: ColumnLayout {
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 68
            color: AppTheme.editorPanel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                spacing: AppTheme.spacing16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        objectName: "themeEditorTitle"
                        text: qsTr("Custom theme")
                        color: AppTheme.editorText
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: root.store.overrideCount === 0
                              ? qsTr("Click any part of the sample window, or a role on the left.")
                              : qsTr("%n colour(s) changed. Everything else follows the theme you started from.",
                                     "custom theme, count of edited roles",
                                     root.store.overrideCount)
                        color: AppTheme.editorTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }

                // The actions, in their own Row.
                //
                // A Row and NOT more RowLayout children: a linear layout
                // hands its slack to items it thinks can grow, and the
                // buttons ended up separated by a couple of hundred pixels
                // each ("buttons on the top are spaced apart very widely").
                // A Row positions children at their implicit widths with a
                // fixed gap and skips invisible ones, which is exactly what
                // a button cluster wants.
                Row {
                    // Pinned to the top-RIGHT corner explicitly. Relying on
                    // the title column's fillWidth to push the cluster over
                    // works only for as long as nothing else in this header
                    // ever grows.
                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                    spacing: AppTheme.spacing8

                    // Reset-to-default, with its confirmation inline. A second
                    // dialog on top of this one would be painted by the shared
                    // dialog shell, which is exactly what this surface cannot
                    // depend on.
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.confirmingReset
                        text: qsTr("Reset every colour?")
                        color: AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        rightPadding: AppTheme.spacing4
                    }
                    EditorButton {
                        objectName: "themeResetConfirmButton"
                        visible: root.confirmingReset
                        danger: true
                        text: qsTr("Reset to default")
                        onClicked: {
                            root.store.resetAll()
                            root.confirmingReset = false
                            if (root.editingRole.length > 0)
                                picker.load(root.effectiveColor(root.editingRole))
                        }
                    }
                    EditorButton {
                        visible: root.confirmingReset
                        text: qsTr("Keep")
                        onClicked: root.confirmingReset = false
                    }
                    EditorButton {
                        objectName: "themeResetAllButton"
                        visible: !root.confirmingReset
                        enabled: root.store.overrideCount > 0
                        text: qsTr("Reset to default")
                        onClicked: root.confirmingReset = true
                    }

                    // Applying is a separate decision from authoring: the
                    // preview below renders the custom palette whether or not
                    // the running application uses it, so a theme can be built
                    // and looked at before it takes over the window.
                    EditorButton {
                        objectName: "themeApplyButton"
                        visible: app.settings.theme !== 12
                        primary: true
                        text: qsTr("Use this theme")
                        onClicked: app.settings.theme = 12
                    }
                    EditorButton {
                        objectName: "themeEditorDoneButton"
                        primary: app.settings.theme === 12
                        text: qsTr("Done")
                        onClicked: root.close()
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: AppTheme.editorBorder
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Roles ────────────────────────────────────────────────────
            Rectangle {
                Layout.preferredWidth: 330
                Layout.minimumWidth: 330
                Layout.maximumWidth: 330
                Layout.fillHeight: true
                color: AppTheme.editorPanel

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing16
                    spacing: AppTheme.spacing8

                    // ── Your themes ──────────────────────────────────
                    Label {
                        text: qsTr("Your themes")
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6

                        Repeater {
                            model: root.store.themes
                            delegate: Rectangle {
                                id: themeChip
                                required property var modelData
                                readonly property bool current:
                                    root.store.activeThemeId === modelData.id
                                objectName: "customThemeChip_" + modelData.id
                                implicitWidth: Math.min(
                                    296, themeChipLabel.implicitWidth
                                         + AppTheme.spacing12 * 2)
                                implicitHeight: 30
                                radius: AppTheme.radiusPill
                                color: current ? AppTheme.editorAccent
                                     : themeChipHover.containsMouse
                                       ? AppTheme.editorSelection
                                       : AppTheme.editorInset
                                border.width: 1
                                border.color: current ? AppTheme.editorAccent
                                                      : AppTheme.editorBorder

                                Label {
                                    id: themeChipLabel
                                    anchors.centerIn: parent
                                    width: Math.min(implicitWidth,
                                                    themeChip.width
                                                    - AppTheme.spacing12 * 2)
                                    text: themeChip.modelData.name.length > 0
                                          ? themeChip.modelData.name
                                          : qsTr("Untitled")
                                    textFormat: Text.PlainText
                                    color: themeChip.current
                                           ? AppTheme.editorAccentInk
                                           : AppTheme.editorText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                }
                                MouseArea {
                                    id: themeChipHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    Accessible.role: Accessible.Button
                                    Accessible.name: themeChip.modelData.name
                                    onClicked: {
                                        root.store.activeThemeId =
                                            themeChip.modelData.id
                                        root.editingRole = ""
                                    }
                                }
                            }
                        }
                    }

                    // The active theme's name, edited in place. A theme people
                    // are meant to SHARE needs a name that says what it is.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing4
                        implicitHeight: 32
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: nameField.activeFocus ? 2 : 1
                        border.color: nameField.activeFocus
                                      ? AppTheme.editorAccent
                                      : AppTheme.editorBorderStrong

                        TextInput {
                            id: nameField
                            objectName: "customThemeNameField"
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            verticalAlignment: TextInput.AlignVCenter
                            color: AppTheme.editorText
                            selectionColor: AppTheme.editorAccent
                            selectedTextColor: AppTheme.editorAccentInk
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            maximumLength: 48
                            Accessible.role: Accessible.EditableText
                            Accessible.name: qsTr("Theme name")
                            text: root.store.name
                            // Committed AS IT IS TYPED. editingFinished alone
                            // meant Enter or a focus change, and neither is
                            // reliably reached here: clicking a colour region
                            // in the sample window is a MouseArea that takes
                            // no active focus, and pressing Done destroys the
                            // field. A name typed and then clicked away from
                            // was simply lost.
                            //
                            // onTextEdited, not onTextChanged: it fires for
                            // USER edits only, so the store write can never be
                            // triggered by the `text` binding itself. Writing
                            // the store re-evaluates that binding with the
                            // same string, which setText early-returns on, so
                            // the caret does not move. setName only truncates
                            // at 48 (the field's own maximumLength) and never
                            // trims, so nothing snaps back under the cursor
                            // mid-word.
                            onTextEdited: root.store.name = text
                            onEditingFinished: root.store.name = text
                            Label {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                visible: nameField.text.length === 0
                                text: qsTr("Name this theme")
                                color: AppTheme.editorTextMuted
                                font: nameField.font
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6

                        EditorButton {
                            objectName: "customThemeNewButton"
                            text: qsTr("New")
                            onClicked: {
                                root.store.createTheme("")
                                root.editingRole = ""
                            }
                        }
                        EditorButton {
                            objectName: "customThemeDuplicateButton"
                            text: qsTr("Duplicate")
                            enabled: root.store.exists
                            onClicked: root.store.duplicateActiveTheme("")
                        }
                        EditorButton {
                            objectName: "customThemeShareButton"
                            text: qsTr("Share")
                            enabled: root.store.exists
                            onClicked: {
                                var payload = root.store.exportTheme(
                                    root.store.activeThemeId)
                                if (payload.length === 0)
                                    return
                                themeClipboard.text = payload
                                themeClipboard.selectAll()
                                themeClipboard.copy()
                                themeClipboard.text = ""
                                root.notice =
                                    qsTr("Theme copied — paste it to share it.")
                            }
                        }
                        EditorButton {
                            objectName: "customThemeImportButton"
                            text: qsTr("Import")
                            onClicked: {
                                root.importing = !root.importing
                                root.importError = ""
                            }
                        }
                        EditorButton {
                            objectName: "customThemeDeleteButton"
                            text: qsTr("Delete")
                            danger: true
                            enabled: root.store.exists
                            onClicked: {
                                root.store.deleteTheme(root.store.activeThemeId)
                                root.editingRole = ""
                            }
                        }
                    }

                    // Paste-a-theme row, shown only while importing.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: root.importing
                        implicitHeight: 32
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: importField.activeFocus ? 2 : 1
                        border.color: importField.activeFocus
                                      ? AppTheme.editorAccent
                                      : AppTheme.editorBorderStrong

                        TextInput {
                            id: importField
                            objectName: "customThemeImportField"
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            verticalAlignment: TextInput.AlignVCenter
                            clip: true
                            color: AppTheme.editorText
                            selectionColor: AppTheme.editorAccent
                            selectedTextColor: AppTheme.editorAccentInk
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                            // A shared theme is a single compact line; the cap
                            // is far above any real one and stops a paste of
                            // something else entirely from being held here.
                            maximumLength: 8192
                            Accessible.role: Accessible.EditableText
                            Accessible.name: qsTr("Paste a shared theme")
                            onAccepted: root.applyImport()
                            Label {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                visible: importField.text.length === 0
                                text: qsTr("Paste a shared theme, then Enter")
                                color: AppTheme.editorTextMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.importing && root.importError.length > 0
                        text: root.importError
                        wrapMode: Text.WordWrap
                        color: AppTheme.editorDanger
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.notice.length > 0
                        text: root.notice
                        wrapMode: Text.WordWrap
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing4
                        implicitHeight: 1
                        color: AppTheme.editorBorder
                    }

                    Label {
                        text: qsTr("Start from")
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }

                    // Base-theme chips, each painting its own palette. A
                    // combo box shows a NAME; this shows the thing the name
                    // refers to, which is the only useful question here.
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6

                        Repeater {
                            // 12 is this theme itself: a cycle QML resolves as
                            // an undefined palette rather than as an error.
                            model: AppTheme.themeList.filter((t) => t.id !== 12
                                                             && t.id !== 0)
                            delegate: Rectangle {
                                id: baseChip
                                required property var modelData
                                readonly property bool current:
                                    root.store.baseTheme === modelData.id
                                readonly property var chipPal:
                                    AppTheme.paletteForTheme(modelData.id)
                                objectName: "themeBaseChip_" + modelData.id
                                width: 130
                                height: 34
                                radius: AppTheme.radiusMd
                                color: current ? AppTheme.editorSelection
                                     : chipHover.containsMouse ? AppTheme.editorInset
                                                               : "transparent"
                                border.width: current ? 2 : 1
                                border.color: current ? AppTheme.editorAccent
                                                      : AppTheme.editorBorder

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: AppTheme.spacing6
                                    anchors.rightMargin: AppTheme.spacing6
                                    spacing: AppTheme.spacing6

                                    Row {
                                        spacing: 1
                                        Repeater {
                                            model: ["sidebar", "background", "accent"]
                                            delegate: Rectangle {
                                                required property string modelData
                                                width: 6
                                                height: 20
                                                radius: 1
                                                color: baseChip.chipPal[modelData]
                                            }
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: baseChip.modelData.name
                                        textFormat: Text.PlainText
                                        color: AppTheme.editorText
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMeta
                                        elide: Label.ElideRight
                                    }
                                }

                                MouseArea {
                                    id: chipHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.store.baseTheme = baseChip.modelData.id
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing6
                        implicitHeight: 1
                        color: AppTheme.editorBorder
                    }

                    Flickable {
                        id: roleScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: width
                        contentHeight: roleColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                            contentItem: Rectangle {
                                implicitWidth: 5
                                radius: 2.5
                                color: AppTheme.editorBorderStrong
                            }
                        }

                        ColumnLayout {
                            id: roleColumn
                            width: roleScroll.width
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
                                        textFormat: Text.PlainText
                                        color: AppTheme.editorTextMuted
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
                                            implicitHeight: 38
                                            radius: AppTheme.radiusSm
                                            color: editing ? AppTheme.editorSelection
                                                 : roleHover.containsMouse
                                                   ? AppTheme.editorInset
                                                   : "transparent"

                                            MouseArea {
                                                id: roleHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.beginEdit(
                                                    roleRow.modelData.key,
                                                    roleRow.modelData.label)
                                            }

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: AppTheme.spacing8
                                                anchors.rightMargin: AppTheme.spacing6
                                                spacing: AppTheme.spacing8

                                                Rectangle {
                                                    objectName: "themeSwatch_"
                                                                + roleRow.modelData.key
                                                    implicitWidth: 24
                                                    implicitHeight: 24
                                                    radius: AppTheme.radiusSm
                                                    color: root.effectiveColor(
                                                        roleRow.modelData.key)
                                                    // A changed role is marked
                                                    // on the swatch, so the
                                                    // list reads as a diff at
                                                    // a glance.
                                                    border.width: root.isOverridden(
                                                        roleRow.modelData.key) ? 2 : 1
                                                    border.color: root.isOverridden(
                                                        roleRow.modelData.key)
                                                        ? AppTheme.editorAccent
                                                        : AppTheme.editorBorderStrong
                                                }

                                                Label {
                                                    // Remote or externally chosen text: never markup.
                                                    textFormat: Text.PlainText
                                                    Layout.fillWidth: true
                                                    text: roleRow.modelData.label
                                                    color: AppTheme.editorText
                                                    font.family: AppTheme.uiFont
                                                    font.pixelSize: AppTheme.textMeta
                                                    elide: Label.ElideRight
                                                }

                                                Rectangle {
                                                    objectName: "themeRoleReset_"
                                                                + roleRow.modelData.key
                                                    visible: root.isOverridden(
                                                        roleRow.modelData.key)
                                                    implicitWidth: 24
                                                    implicitHeight: 24
                                                    radius: AppTheme.radiusSm
                                                    color: resetHover.containsMouse
                                                           ? AppTheme.editorSelection
                                                           : "transparent"
                                                    Icon {
                                                        anchors.centerIn: parent
                                                        name: "undo"
                                                        size: 14
                                                        color: AppTheme.editorTextSecondary
                                                    }
                                                    MouseArea {
                                                        id: resetHover
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        Accessible.role: Accessible.Button
                                                        Accessible.name:
                                                            qsTr("Reset %1 to the base theme")
                                                                .arg(roleRow.modelData.label)
                                                        onClicked: {
                                                            root.store.resetColor(
                                                                roleRow.modelData.key)
                                                            if (root.editingRole
                                                                    === roleRow.modelData.key)
                                                                picker.load(root.effectiveColor(
                                                                    roleRow.modelData.key))
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Preview ──────────────────────────────────────────────────
            Item {
                id: previewFrame
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing20
                    spacing: AppTheme.spacing8

                    Item {
                        id: previewStage
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        // The preview renders at its NATURAL size and is
                        // scaled DOWN to fit, never up. Stretching a shell
                        // mock re-flows it into proportions the real window
                        // never has — a 1200px-wide room list, a two-line
                        // composer — and then the user is judging colours on
                        // a layout that does not exist. Scaling up would
                        // blur it: Item.scale renders at the original
                        // resolution first.
                        ThemePreviewDemo {
                            id: preview
                            objectName: "themePreviewDemo"
                            pal: root.previewPalette
                            // The user's OWN layout. Previewing the Classic
                            // column to somebody who runs Channels shows them
                            // where a colour lands in a column they never see.
                            channels: app.settings
                                      && app.settings.roomNavigationLayout === 1
                            highlightRole: root.editingRole
                            width: implicitWidth
                            height: implicitHeight
                            transformOrigin: Item.TopLeft
                            scale: Math.min(1.0,
                                            previewStage.width / implicitWidth,
                                            previewStage.height / implicitHeight)
                            x: (previewStage.width - implicitWidth * scale) / 2
                            y: (previewStage.height - implicitHeight * scale) / 2
                            onRegionActivated: (role) =>
                                root.beginEdit(role, root.labelForRole(role))
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("A sample window, not one of your rooms. Click a part of it to recolour it.")
                        color: AppTheme.editorTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }

            // ── Picker ───────────────────────────────────────────────────
            // Occupies the column whether or not a role is open, so choosing a
            // role does not re-flow the preview next to it.
            Rectangle {
                Layout.preferredWidth: 320
                Layout.minimumWidth: 320
                Layout.maximumWidth: 320
                Layout.fillHeight: true
                color: AppTheme.editorPanel

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing16
                    spacing: AppTheme.spacing12

                    ColorPickerPanel {
                        id: picker
                        objectName: "themeColorPicker"
                        Layout.fillWidth: true
                        visible: root.editingRole.length > 0
                        title: root.editingLabel
                        subtitle: root.hintForRole(root.editingRole)
                        canReset: root.editingRole.length > 0
                                  && root.isOverridden(root.editingRole)
                        suggestions: root.paletteSwatches
                        onPicked: (value) => {
                            if (root.editingRole.length > 0)
                                root.store.setColor(root.editingRole,
                                                    root.toHex(value))
                        }
                        onResetRequested: {
                            if (root.editingRole.length > 0) {
                                root.store.resetColor(root.editingRole)
                                picker.load(root.effectiveColor(root.editingRole))
                            }
                        }
                        onClosed: root.editingRole = ""
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.editingRole.length === 0
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Nothing selected")
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightStrong
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Click a part of the sample window in the middle, or a role in the list on the left, and its colour opens here.")
                            color: AppTheme.editorTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                Rectangle {
                    anchors.left: parent.left
                    height: parent.height
                    width: 1
                    color: AppTheme.editorBorder
                }
            }
        }
    }

    // The base theme's own colours, offered in the picker. Building a theme
    // almost always means reusing a tone that is already in it — a hand-typed
    // near-miss is how a palette loses its coherence.
    readonly property var paletteSwatches: {
        var pal = AppTheme.paletteForTheme(root.store.baseTheme)
        var keys = ["background", "sidebar", "rail", "surface", "cardElevated",
                    "hover", "selected", "border", "borderStrong",
                    "inputBackground", "accent", "link", "textPrimary",
                    "textSecondary", "textMuted", "ownBubble", "otherBubble"]
        var out = []
        var seen = {}
        for (var i = 0; i < keys.length; ++i) {
            var v = pal[keys[i]]
            if (v === undefined)
                continue
            var hex = root.toHex(Qt.color(String(v)))
            if (seen[hex] !== undefined)
                continue
            seen[hex] = true
            out.push(hex)
        }
        return out
    }
}
