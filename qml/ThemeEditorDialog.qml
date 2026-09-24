import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Custom theme editor (Settings → Appearance → Custom theme): a full-window
// workspace with the editable roles, a live preview of the whole app, and the
// colour picker. The picker is inline rather than a ColorDialog so it never
// covers the preview.
//
// A Popup parented to Overlay.overlay with explicit geometry: AppDialog centres
// in its parent, which here is a scrolled settings column.
//
// The chrome uses AppTheme's invariant editor tokens and draws its own controls
// (see `editorCanvas` in AppTheme.qml): the shared controls follow the selected
// theme, and a theme being edited can make them unreadable.
//
// No draft: every change commits to CustomThemeStore immediately, so there is
// no Save button and Reset is the undo.
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

    // Create the theme on open so there is something to edit; a theme with no
    // overrides simply follows its base.
    onOpened: if (!root.store.exists) root.store.createTheme("")

    readonly property var store: app.customTheme

    // Geometry. Side columns are ranges rather than fixed widths, so the
    // preview stays usable on ordinary windows. Below `compact` the picker
    // overlays the role column instead of taking a third column; the picker
    // must never cover the preview, and the role list is not needed while a
    // role is open. The preview never reflows when a role is selected: in wide
    // mode the third column is always reserved (holding the readability report
    // when no role is open), and in compact mode the picker is an overlay. The
    // breakpoint (1380) is where three columns still give the preview a usable
    // scale; below it they fold, and the preview visibly grows as the column
    // folds away.
    readonly property int headerHeight: 68
    readonly property bool compact: root.width < 1380
    readonly property int workColumnWidth:
        Math.max(268, Math.min(330, Math.round(root.width * 0.235)))
    // Slightly tighter than the role column: ColorPickerPanel's natural width
    // is 288 and everything in it stretches.
    readonly property int pickerColumnWidth:
        Math.max(272, Math.min(304, Math.round(root.width * 0.222)))
    // Where the picker sits while open; in compact mode it covers the role
    // column.
    readonly property int pickerPanelX:
        root.compact ? 0 : root.width - root.pickerColumnWidth
    readonly property int pickerPanelWidth:
        root.compact ? root.workColumnWidth : root.pickerColumnWidth
    // In compact mode the panel must be asked for, and the header badge opens
    // it for the readability report, so the report is reachable at every width.
    property bool reportOpen: false
    readonly property bool pickerPanelVisible:
        !root.compact || root.editingRole.length > 0 || root.reportOpen
    // True while the panel covers the role column; everything underneath must
    // stop taking input.
    readonly property bool pickerPanelCovers:
        root.compact && root.pickerPanelVisible

    // Readability findings for the palette as the app would paint it:
    // previewPalette is the same resolved object the preview renders, so
    // inherited colours are checked too. See CustomThemeStore for what is
    // checked. Throttled: graded on every drag sample, the palette crossed into
    // C++ several times per sample and the report rebuilt its delegates,
    // costing about a second of CPU per drag. A trailing 120 ms throttle
    // (auditTimer starts only when not running) updates during the drag and
    // always lands the final sample; a debounce would freeze the numbers for
    // the whole gesture.
    property var auditPalette: root.previewPalette
    onPreviewPaletteChanged: if (!auditTimer.running) auditTimer.start()
    Timer {
        id: auditTimer
        interval: 120
        onTriggered: root.auditPalette = root.previewPalette
    }

    readonly property var readabilityReport: root.store.audit(root.auditPalette)
    readonly property int readabilityProblems: root.readabilityReport.length

    // What could not be checked is not what passed. The store refuses to grade
    // a translucent colour (its contrast is unknowable), so report those
    // separately rather than letting the badge say "Readable".
    readonly property var readabilitySkipped:
        root.store.auditSkipped(root.auditPalette, "")
    readonly property int readabilityUnchecked: root.readabilitySkipped.length
    // "Your colour is see-through" (the user's choice) and "this build has no
    // such colour" (our bug) are counted and reported separately; a palette can
    // hold both.
    readonly property int readabilityMissing: {
        var rows = root.readabilitySkipped
        var n = 0
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].reason === "missing")
                ++n
        }
        return n
    }
    readonly property int readabilityTranslucent:
        root.readabilityUnchecked - root.readabilityMissing

    // The role open in the picker. Held on the dialog: a Repeater row can be
    // destroyed while the picker is open.
    property string editingRole: ""
    property string editingLabel: ""
    property bool confirmingReset: false
    // The base-theme grid, collapsed by default (see below).
    property bool basesExpanded: false
    // Filter for the role list.
    property string roleFilter: ""
    // Import/share state. `notice` is transient and cleared by the timer below.
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
        // Report the problem count of an imported theme, since nobody has
        // checked it. Deferred a frame because the store has only just emitted
        // its change. Force the audit first: the throttle assigns auditPalette
        // imperatively, which destroyed its binding, so otherwise the count
        // would describe the previous theme.
        root.auditPalette = root.previewPalette
        importNoticeTimer.restart()
    }

    Timer {
        id: importNoticeTimer
        interval: 0
        onTriggered: root.notice =
            root.readabilityProblems > 0
            ? qsTr("Theme imported. %n thing(s) in it will be hard to read.",
                   "custom theme readability", root.readabilityProblems)
            : qsTr("Theme imported.")
    }

    // Clipboard shuttle for Share, as used elsewhere in the app.
    TextEdit {
        id: themeClipboard
        visible: false
        width: 0
        height: 0
    }

    // The palette the preview paints, resolved by id so it shows the custom
    // theme whether or not it is active. Uses the same resolver as the running
    // app (Main.qml keeps AppTheme.customOverrides/customBase bound to the
    // store).
    readonly property var previewPalette: AppTheme.paletteForTheme(12)

    // The two hot inputs, read once per change instead of per row: store.colors
    // returns a QVariantMap by value (a fresh conversion per read), and
    // paletteForTheme() builds a fresh object per call. The role list
    // instantiates all 26 rows and repaints on every drag sample.
    readonly property var overrideColors: root.store.colors
    readonly property var basePalette:
        AppTheme.paletteForTheme(root.store.baseTheme)

    function effectiveColor(rolekey) {
        var overrides = root.overrideColors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var pal = root.basePalette
        // paletteForTheme uses semantic names; a few store keys differ (inputBg
        // -> inputBackground, mention -> mentionBadge, reaction ->
        // reactionBackground).
        var alias = root.storeKeyAliases[rolekey]
        var lookup = alias !== undefined ? alias : rolekey
        if (pal[lookup] !== undefined)
            return pal[lookup]
        return AppTheme.editorTextMuted
    }

    // The store's own alias map, read once.
    // everyCheckGradesTheColourItsRoleWouldEdit keeps the two spellings in
    // agreement.
    readonly property var storeKeyAliases: root.store.roleAliases()

    function isOverridden(rolekey) {
        var overrides = root.overrideColors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    // One entry point for pointer and keyboard.
    function openReport() {
        root.editingRole = ""
        root.reportOpen = true
    }

    function beginEdit(key, label) {
        root.editingRole = key
        root.editingLabel = label
        // The panel holds one thing at a time; opening a colour takes it.
        root.reportOpen = false
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

    // CustomThemeStore stores #RRGGBB; a QML color's toString() is #AARRGGBB.
    function toHex(c) {
        function two(v) {
            var s = Math.round(v * 255).toString(16).toUpperCase()
            return s.length < 2 ? "0" + s : s
        }
        return "#" + two(c.r) + two(c.g) + two(c.b)
    }

    readonly property string baseThemeName: {
        var list = AppTheme.themeList
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id === root.store.baseTheme)
                return list[i].name
        }
        return ""
    }

    // Groups narrowed by the filter; empty groups are dropped.
    readonly property var roleGroups: {
        var out = []
        var seen = {}
        var needle = root.roleFilter.trim().toLowerCase()
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            if (needle.length > 0
                && list[i].label.toLowerCase().indexOf(needle) < 0
                && list[i].group.toLowerCase().indexOf(needle) < 0
                && list[i].hint.toLowerCase().indexOf(needle) < 0)
                continue
            var g = list[i].group
            if (seen[g] === undefined) {
                seen[g] = out.length
                out.push({ name: g, items: [] })
            }
            out[seen[g]].items.push(list[i])
        }
        return out
    }

    // Self-contained controls, painted in the invariant editor tokens.
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

    // A plain Item: the picker/report panel is positioned, not laid out (it
    // moves column in compact mode), and a child of a ColumnLayout would become
    // a layout item.
    contentItem: Item {

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.headerHeight
            color: AppTheme.editorPanel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                spacing: AppTheme.spacing16

                // Elides: an unelided Label's implicit width would push the
                // button cluster, including Done, off a narrow window.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    Label {
                        objectName: "themeEditorTitle"
                        Layout.fillWidth: true
                        text: qsTr("Custom theme")
                        color: AppTheme.editorText
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightBold
                        elide: Label.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: root.store.overrideCount === 0
                                  ? qsTr("Click any part of the sample window, or a role on the left.")
                                  : qsTr("%n colour(s) changed.",
                                         "custom theme, count of edited roles",
                                         root.store.overrideCount)
                            color: AppTheme.editorTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            elide: Label.ElideRight
                        }

                        // The verdict, always on screen. The full report shares
                        // a panel with the picker, so this badge is the one
                        // place the answer is always visible; clicking it opens
                        // the report.
                        Rectangle {
                            objectName: "themeReadabilityBadge"
                            implicitWidth: verdictLabel.implicitWidth
                                           + AppTheme.spacing8 * 2
                            implicitHeight: 22
                            radius: AppTheme.radiusPill
                            // Shown whenever there is something to say. The
                            // "could not check" clause appears only in compact
                            // mode, where the badge is the sole route to it; in
                            // wide mode the report column already says it, and
                            // a stock Storm base always has one ungradable
                            // pair, so it would qualify every pristine theme.
                            visible: root.store.overrideCount > 0
                                     || root.readabilityProblems > 0
                                     || (root.compact
                                         && root.readabilityUnchecked > 0)
                            color: verdictHover.containsMouse
                                   ? AppTheme.editorSelection
                                   : AppTheme.editorInset
                            border.width: 1
                            border.color: root.readabilityProblems > 0
                                          ? AppTheme.editorDanger
                                          : AppTheme.editorBorderStrong
                            // Keyboard reachable.
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: verdictLabel.text
                            Keys.onPressed: (e) => {
                                if (e.key === Qt.Key_Return
                                    || e.key === Qt.Key_Enter
                                    || e.key === Qt.Key_Space) {
                                    root.openReport()
                                    e.accepted = true
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                visible: parent.activeFocus
                                radius: AppTheme.radiusPill
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.editorAccent
                            }
                            Label {
                                id: verdictLabel
                                anchors.centerIn: parent
                                // A pass that leaves something unchecked says
                                // so (see readabilitySkipped).
                                text: root.readabilityProblems > 0
                                      ? qsTr("%n thing(s) hard to read",
                                             "custom theme readability",
                                             root.readabilityProblems)
                                      : root.readabilityUnchecked > 0
                                        ? qsTr("Readable, %n not checked",
                                               "custom theme readability",
                                               root.readabilityUnchecked)
                                        : qsTr("Readable")
                                color: root.readabilityProblems > 0
                                       ? AppTheme.editorDanger
                                       : AppTheme.editorTextSecondary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                            }
                            MouseArea {
                                id: verdictHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openReport()
                            }
                        }
                    }
                }

                // A Row rather than RowLayout children: a layout hands slack to
                // its items and spread the buttons apart.
                Row {
                    // Pinned to the top-right explicitly.
                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                    spacing: AppTheme.spacing8

                    // Reset with an inline confirmation; a second dialog would
                    // use the shared dialog shell, which this surface cannot
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

                    // Applying is separate from authoring: the preview shows
                    // the custom palette whether or not the app uses it.
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

            // Roles
            Rectangle {
                id: workColumn
                Layout.preferredWidth: root.workColumnWidth
                Layout.minimumWidth: root.workColumnWidth
                Layout.maximumWidth: root.workColumnWidth
                Layout.fillHeight: true
                color: AppTheme.editorPanel

                // Nothing under the overlay may take input: in compact mode the
                // picker panel covers this column and a plain Rectangle accepts
                // no buttons, so clicks fell through to hidden rows. `enabled:
                // false` also removes them from the tab chain.
                enabled: !root.pickerPanelCovers

                // A seam on this side too, matching the picker column's border.
                Rectangle {
                    anchors.right: parent.right
                    height: parent.height
                    width: 1
                    color: AppTheme.editorBorder
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing16
                    spacing: AppTheme.spacing8

                    // Your themes
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

                    // The theme's name, edited in place; a shared theme needs a
                    // name.
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
                            // Committed as typed: editingFinished is not
                            // reliably reached (a click on the sample window
                            // takes no focus, and Done destroys the field).
                            // onTextEdited fires for user edits only, so the
                            // text binding cannot trigger a write, and setName
                            // only truncates at 48 and never trims.
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
                            // Far above any real shared theme; stops holding an
                            // unrelated paste.
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

                    // Collapsed by default: the full grid takes about 235px
                    // permanently for a control used once per theme, starving
                    // the role list. Collapsed, the current base is still shown
                    // as its own chip.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            text: qsTr("Start from")
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                        }
                        Item { Layout.fillWidth: true }
                        EditorButton {
                            objectName: "themeBasesToggleButton"
                            implicitHeight: 24
                            text: root.basesExpanded ? qsTr("Done")
                                                     : qsTr("Change")
                            onClicked: root.basesExpanded = !root.basesExpanded
                        }
                    }

                    // The current base, shown while collapsed.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: !root.basesExpanded
                        implicitHeight: 34
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: 1
                        border.color: AppTheme.editorBorder
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing8
                            Row {
                                spacing: 1
                                Repeater {
                                    model: ["sidebar", "background", "accent"]
                                    delegate: Rectangle {
                                        required property string modelData
                                        width: 8
                                        height: 20
                                        radius: 2
                                        color: root.basePalette[modelData]
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: root.baseThemeName
                                color: AppTheme.editorText
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                        }
                    }

                    // Base-theme chips painted in their own palettes.
                    Flow {
                        Layout.fillWidth: true
                        visible: root.basesExpanded
                        spacing: AppTheme.spacing6

                        Repeater {
                            // 12 is this theme itself; a cycle resolves as an
                            // undefined palette.
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
                                                width: 8
                                                height: 20
                                                radius: 2
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

                                // Keyboard reachable.
                                activeFocusOnTab: true
                                Accessible.role: Accessible.RadioButton
                                Accessible.name: baseChip.modelData.name
                                Accessible.checked: baseChip.current
                                Keys.onPressed: (e) => {
                                    if (e.key === Qt.Key_Return
                                        || e.key === Qt.Key_Enter
                                        || e.key === Qt.Key_Space) {
                                        root.store.baseTheme = baseChip.modelData.id
                                        e.accepted = true
                                    }
                                }
                                // Inset, not outset: an outset ring breaks
                                // neighbours' spacing. Same inset as
                                // EditorButton.
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    visible: baseChip.activeFocus
                                    radius: AppTheme.radiusSm
                                    color: "transparent"
                                    border.width: 2
                                    border.color: AppTheme.editorAccent
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

                    // Filter field: with 26 roles, typing beats scrolling,
                    // especially on small windows.
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 30
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: filterField.activeFocus ? 2 : 1
                        border.color: filterField.activeFocus
                                      ? AppTheme.editorAccent
                                      : AppTheme.editorBorderStrong

                        TextInput {
                            id: filterField
                            objectName: "themeRoleFilterField"
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            verticalAlignment: TextInput.AlignVCenter
                            clip: true
                            activeFocusOnTab: true
                            color: AppTheme.editorText
                            selectionColor: AppTheme.editorAccent
                            selectedTextColor: AppTheme.editorAccentInk
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            maximumLength: 48
                            Accessible.role: Accessible.EditableText
                            Accessible.name: qsTr("Find a colour")
                            onTextEdited: root.roleFilter = text
                            // Escape must still close the editor: Keys accepts
                            // the event before a named handler runs, so an
                            // unconditional handler would swallow it. Clear the
                            // filter if there is one; otherwise pass the key
                            // on.
                            Keys.onEscapePressed: (event) => {
                                if (text.length === 0) {
                                    event.accepted = false
                                    return
                                }
                                text = ""
                                root.roleFilter = ""
                            }
                            Label {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                visible: filterField.text.length === 0
                                text: qsTr("Find a colour")
                                color: AppTheme.editorTextMuted
                                font: filterField.font
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.roleFilter.trim().length > 0
                                 && root.roleGroups.length === 0
                        text: qsTr("Nothing matches that.")
                        color: AppTheme.editorTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
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

                                    // editorTextSecondary: the muted ink was
                                    // nearly invisible, leaving the list
                                    // without navigation.
                                    Label {
                                        Layout.topMargin: AppTheme.spacing6
                                        text: modelData.name
                                        textFormat: Text.PlainText
                                        color: AppTheme.editorTextSecondary
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
                                            readonly property bool overridden:
                                                root.isOverridden(modelData.key)
                                            readonly property color resolved:
                                                Qt.color(String(
                                                    root.effectiveColor(
                                                        modelData.key)))
                                            readonly property string hex:
                                                root.toHex(resolved)
                                            // A translucent inherited value (e.g.
                                            // Storm's `hover`) paints with its alpha
                                            // in the swatch, prints as an opaque hex,
                                            // and is not graded by the audit; this
                                            // flag lets the row explain that.
                                            readonly property bool seeThrough:
                                                resolved.a < 0.999
                                            Layout.fillWidth: true
                                            implicitHeight: 38
                                            radius: AppTheme.radiusSm
                                            color: editing ? AppTheme.editorSelection
                                                 : roleHover.containsMouse
                                                   ? AppTheme.editorInset
                                                   : "transparent"

                                            // Keyboard reachable.
                                            activeFocusOnTab: true
                                            Accessible.role: Accessible.Button
                                            Accessible.name:
                                                roleRow.modelData.label
                                            Accessible.description:
                                                roleRow.modelData.hint
                                            Keys.onPressed: (e) => {
                                                if (e.key === Qt.Key_Return
                                                    || e.key === Qt.Key_Enter
                                                    || e.key === Qt.Key_Space) {
                                                    root.beginEdit(
                                                        roleRow.modelData.key,
                                                        roleRow.modelData.label)
                                                    e.accepted = true
                                                }
                                            }
                                            Rectangle {
                                                anchors.fill: parent
                                                visible: roleRow.activeFocus
                                                radius: AppTheme.radiusSm
                                                color: "transparent"
                                                border.width: 2
                                                border.color: AppTheme.editorAccent
                                            }
                                            // A Flickable does not follow focus, so
                                            // scroll the focused row into view.
                                            // mapToItem because the row's parent is
                                            // its group's ColumnLayout. Clamped, since
                                            // assigning contentY does not clamp.
                                            onActiveFocusChanged: {
                                                if (!activeFocus)
                                                    return
                                                var top = mapToItem(roleColumn,
                                                                    0, 0).y
                                                var want = roleScroll.contentY
                                                if (top < want)
                                                    want = top
                                                else if (top + height
                                                         > want + roleScroll.height)
                                                    want = top + height
                                                           - roleScroll.height
                                                roleScroll.contentY =
                                                    Math.max(0, Math.min(
                                                        want,
                                                        roleScroll.contentHeight
                                                        - roleScroll.height))
                                            }

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

                                                // The swatch is the data; the "changed"
                                                // mark is a separate dot rather than an
                                                // accent ring that would dominate the
                                                // list. The dot sits inside the cell's own
                                                // bounds (the swatch is inset 4px), so
                                                // nothing overhangs.
                                                Item {
                                                    implicitWidth: 30
                                                    implicitHeight: 30
                                                    Rectangle {
                                                        objectName: "themeSwatch_"
                                                            + roleRow.modelData.key
                                                        x: 0
                                                        y: 4
                                                        width: 26
                                                        height: 26
                                                        radius: AppTheme.radiusSm
                                                        color: root.effectiveColor(
                                                            roleRow.modelData.key)
                                                        border.width: 1
                                                        border.color:
                                                            AppTheme.editorBorderStrong
                                                    }
                                                    Rectangle {
                                                        objectName: "themeRoleChangedDot_"
                                                            + roleRow.modelData.key
                                                        visible: roleRow.overridden
                                                        x: parent.width - width
                                                        y: 0
                                                        width: 8
                                                        height: 8
                                                        radius: 4
                                                        color: AppTheme.editorAccent
                                                        border.width: 1
                                                        border.color: AppTheme.editorPanel
                                                    }
                                                }

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 0
                                                    Label {
                                                        // Untrusted text: never markup.
                                                        textFormat: Text.PlainText
                                                        Layout.fillWidth: true
                                                        text: roleRow.modelData.label
                                                        color: AppTheme.editorText
                                                        font.family: AppTheme.uiFont
                                                        font.pixelSize: AppTheme.textMeta
                                                        elide: Label.ElideRight
                                                    }
                                                    // The value, shown on every row so a tone
                                                    // can be read and copied between roles
                                                    // without opening the picker.
                                                    Label {
                                                        objectName: "themeRoleHex_"
                                                            + roleRow.modelData.key
                                                        textFormat: Text.PlainText
                                                        Layout.fillWidth: true
                                                        text: roleRow.seeThrough
                                                              ? qsTr("%1 · see-through")
                                                                .arg(roleRow.hex)
                                                              : roleRow.hex
                                                        color: AppTheme.editorTextMuted
                                                        font.family: AppTheme.monoFont
                                                        font.pixelSize: AppTheme.menuSectionSize
                                                        elide: Label.ElideRight
                                                    }
                                                }

                                                Rectangle {
                                                    objectName: "themeRoleReset_"
                                                                + roleRow.modelData.key
                                                    visible: roleRow.overridden
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

            // Preview
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

                        // Rendered at natural size and scaled down to fit,
                        // never up: stretching reflows the mock into
                        // proportions the real window never has, and scaling up
                        // blurs it.
                        ThemePreviewDemo {
                            id: preview
                            objectName: "themePreviewDemo"
                            pal: root.previewPalette
                            // The user's own layout, so colours land where they
                            // will actually be seen.
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

            // Picker slot: reserved, not occupied. The picker is positioned
            // manually below (it overlays the role column in compact mode,
            // keeping one instance of its HSV state); this item keeps the
            // preview from reflowing in wide mode.
            Item {
                Layout.preferredWidth: root.pickerColumnWidth
                Layout.minimumWidth: root.pickerColumnWidth
                Layout.maximumWidth: root.pickerColumnWidth
                Layout.fillHeight: true
                visible: !root.compact
            }
        }
    }

    // The picker/report panel, one instance with two homes. Wide: the reserved
    // third column, showing the picker when a role is open and the readability
    // report otherwise. Compact: an overlay on the role column while editing.
    Rectangle {
        id: pickerPanel
        x: root.pickerPanelX
        y: root.headerHeight
        width: root.pickerPanelWidth
        height: Math.max(0, root.height - root.headerHeight)
        visible: root.pickerPanelVisible
        color: AppTheme.editorPanel

        // Event sink, declared first so it is the bottom sibling: a Rectangle
        // accepts no mouse buttons, so without it presses, hover, wheel and
        // cursor shape fell through to the column underneath. Controls above it
        // still win.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
            onWheel: (wheel) => wheel.accepted = true
        }

        // The seam is positioned, not anchored: the panel is built while width
        // is 0 (compact), and switching anchors later let the anchor system
        // overwrite the `width: 1` binding, turning the rule into a full-width
        // slab.
        Rectangle {
            x: root.compact ? parent.width - width : 0
            y: 0
            width: 1
            height: parent.height
            color: AppTheme.editorBorder
        }

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

            // Live readout: every pair the open role takes part in, graded as
            // you drag, passes included, so a number climbing past its bar
            // shows the relationship.
            ColumnLayout {
                id: roleReadability
                objectName: "themeRoleReadability"
                Layout.fillWidth: true
                visible: root.editingRole.length > 0 && roleChecks.count > 0
                spacing: 2

                // Two lines per row at a constant height from FontMetrics: one
                // line truncated the pair description at this column width. A
                // wrapping Label feeding its own row height is a layout loop
                // waiting to happen; FontMetrics depends only on the font, so
                // it adapts to the UI font without folding back.
                FontMetrics {
                    id: checkMetrics
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                // Ceiling per line: Qt lays out each line at an integral
                // height, so two 16.5px lines need 34px, and 33 shows one line
                // elided.
                readonly property int checkRowHeight:
                    2 * Math.ceil(checkMetrics.lineSpacing)

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacing4
                    text: qsTr("Readability")
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                }
                Repeater {
                    id: roleChecks
                    // The throttled palette (see auditPalette). Ungradable
                    // pairs are included as "not checked" rows so the count is
                    // honest.
                    model: root.editingRole.length > 0
                           ? root.store.auditForRole(root.auditPalette,
                                                     root.editingRole)
                             .concat(root.store.auditSkipped(root.auditPalette,
                                                             root.editingRole))
                           : []
                    delegate: RowLayout {
                        id: checkRow
                        required property var modelData
                        // `passes` is absent on a skipped row, never false.
                        readonly property bool graded:
                            modelData.passes !== undefined
                        Layout.fillWidth: true
                        Layout.preferredHeight: roleReadability.checkRowHeight
                        spacing: AppTheme.spacing6
                        Rectangle {
                            implicitWidth: 6
                            implicitHeight: 6
                            radius: 3
                            color: !checkRow.graded ? AppTheme.editorTextMuted
                                 : checkRow.modelData.passes
                                   ? AppTheme.editorAccent
                                   : AppTheme.editorDanger
                        }
                        Label {
                            objectName: "themeRoleCheckLabel"
                            Layout.fillWidth: true
                            // Pinned to the row's constant height and centred,
                            // so rows are uniform.
                            Layout.preferredHeight:
                                roleReadability.checkRowHeight
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            textFormat: Text.PlainText
                            // The check's own sentence (see the phrase field in
                            // CustomThemeStore.cpp).
                            text: checkRow.modelData.label
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            elide: Label.ElideRight
                        }
                        Label {
                            objectName: "themeRoleCheckValue"
                            textFormat: Text.PlainText
                            // A WCAG ratio reads "4.6:1"; a lightness
                            // separation is not a ratio.
                            text: !checkRow.graded
                                  ? qsTr("see-through")
                                  : checkRow.modelData.kind === "ink"
                                    ? qsTr("%1:1").arg(
                                          checkRow.modelData.value.toFixed(1))
                                    : qsTr("ΔL* %1").arg(
                                          checkRow.modelData.value.toFixed(1))
                            color: !checkRow.graded
                                   ? AppTheme.editorTextMuted
                                   : checkRow.modelData.passes
                                     ? AppTheme.editorTextSecondary
                                     : AppTheme.editorDanger
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                        }
                        // The target the number is climbing towards, as in the
                        // report rows.
                        Label {
                            objectName: "themeRoleCheckTarget"
                            visible: checkRow.graded
                            textFormat: Text.PlainText
                            text: qsTr("/ %1").arg(
                                      checkRow.modelData.minimum.toFixed(1))
                            color: AppTheme.editorTextMuted
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
                }
            }

            // The report, when nothing is being edited
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.editingRole.length === 0
                spacing: AppTheme.spacing8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        Layout.fillWidth: true
                        text: root.readabilityProblems > 0
                              ? qsTr("Hard to read")
                              : root.store.overrideCount > 0
                                ? qsTr("Nothing hard to read")
                                : qsTr("Nothing selected")
                        color: root.readabilityProblems > 0
                               ? AppTheme.editorDanger : AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    // Only in compact mode, where the panel is a raised
                    // overlay; in wide mode the column is permanent.
                    Rectangle {
                        objectName: "themeReportCloseButton"
                        visible: root.compact
                        implicitWidth: 26
                        implicitHeight: 26
                        radius: AppTheme.radiusSm
                        color: reportCloseHover.containsMouse
                               ? AppTheme.editorSelection : "transparent"
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Close the readability report")
                        Keys.onPressed: (e) => {
                            if (e.key === Qt.Key_Return
                                || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                                root.reportOpen = false
                                e.accepted = true
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            visible: parent.activeFocus
                            radius: AppTheme.radiusSm
                            color: "transparent"
                            border.width: 2
                            border.color: AppTheme.editorAccent
                        }
                        Icon {
                            anchors.centerIn: parent
                            name: "close"
                            size: 14
                            color: AppTheme.editorTextSecondary
                        }
                        MouseArea {
                            id: reportCloseHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.reportOpen = false
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.readabilityProblems > 0
                          ? qsTr("These are measured against the same rules the built-in themes meet. Click one to fix it.")
                          : root.store.overrideCount > 0
                            ? qsTr("Every text and edge this theme paints clears the bar the built-in themes clear.")
                            : qsTr("Click a part of the sample window in the middle, or a role in the list on the left, and its colour opens here.")
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }

                // What was not checked, stated, so "all passed" is not confused
                // with "all we could check passed".
                Label {
                    objectName: "themeReadabilityUnchecked"
                    visible: root.readabilityTranslucent > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    // Worded to read correctly at n == 1: the English catalog
                    // has no numerus forms for this family, so the source
                    // string is shown at every n.
                    text: qsTr("%n colour(s) could not be checked: a see-through colour reads differently depending on what is behind it.",
                               "custom theme readability",
                               root.readabilityTranslucent)
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }

                // A separate sentence: this means the readability table names a
                // palette key this build does not return, which is our bug.
                // everyReadabilityKeyIsAKeyPaletteForThemeReturns should make
                // it unreachable.
                Label {
                    objectName: "themeReadabilityMissing"
                    visible: root.readabilityMissing > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: qsTr("%n check(s) could not be made: this build has no colour by that name.",
                               "custom theme readability",
                               root.readabilityMissing)
                    color: AppTheme.editorDanger
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }

                Flickable {
                    // Named so a focused row can scroll itself into view; this
                    // list often overflows.
                    id: problemScroll
                    objectName: "themeReadabilityScroll"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.readabilityProblems > 0
                    clip: true
                    contentWidth: width
                    contentHeight: problemColumn.implicitHeight
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
                        id: problemColumn
                        width: parent.width
                        spacing: 2

                        // Three lines per row, still a font-derived constant
                        // (see the live readout).
                        FontMetrics {
                            id: reportMetrics
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        // Ceiling per line (see checkRowHeight).
                        readonly property int rowTextHeight:
                            2 * Math.ceil(reportMetrics.lineSpacing)
                        readonly property int rowHeight:
                            44 + Math.ceil(reportMetrics.lineSpacing)

                        Repeater {
                            model: root.readabilityReport
                            delegate: Rectangle {
                                id: problemRow
                                required property var modelData
                                objectName: "themeReadabilityRow"
                                Layout.fillWidth: true
                                // Fixed height, for the same loop-avoidance
                                // reason; uniform rows also read as a list.
                                implicitHeight: problemColumn.rowHeight
                                radius: AppTheme.radiusSm
                                color: problemHover.containsMouse
                                       ? AppTheme.editorInset : "transparent"

                                // Keyboard reachable.
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: problemText.text
                                // The role rows' clamp, mirrored: assigning
                                // contentY does not clamp.
                                onActiveFocusChanged: {
                                    if (!activeFocus)
                                        return
                                    var top = mapToItem(problemColumn, 0, 0).y
                                    var want = problemScroll.contentY
                                    if (top < want)
                                        want = top
                                    else if (top + height
                                             > want + problemScroll.height)
                                        want = top + height
                                               - problemScroll.height
                                    problemScroll.contentY =
                                        Math.max(0, Math.min(
                                            want,
                                            problemScroll.contentHeight
                                            - problemScroll.height))
                                }
                                Keys.onPressed: (e) => {
                                    if (e.key === Qt.Key_Return
                                        || e.key === Qt.Key_Enter
                                        || e.key === Qt.Key_Space) {
                                        root.beginEdit(
                                            problemRow.modelData.role,
                                            root.labelForRole(
                                                problemRow.modelData.role))
                                        e.accepted = true
                                    }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    visible: problemRow.activeFocus
                                    radius: AppTheme.radiusSm
                                    color: "transparent"
                                    border.width: 2
                                    border.color: AppTheme.editorAccent
                                    z: 1
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: AppTheme.spacing8
                                    anchors.rightMargin: AppTheme.spacing8
                                    spacing: AppTheme.spacing8

                                    // The two failing colours, one on the
                                    // other, at the size they fail at.
                                    Rectangle {
                                        implicitWidth: 30
                                        implicitHeight: 30
                                        radius: AppTheme.radiusSm
                                        // The throttled palette, matching the
                                        // ratio shown beside it.
                                        color: root.auditPalette[
                                            problemRow.modelData.bg]
                                        border.width: 1
                                        border.color: AppTheme.editorBorderStrong
                                        Label {
                                            anchors.centerIn: parent
                                            //: A two-letter type specimen
                                            //: shown on a colour pair to
                                            //: demonstrate its readability.
                                            //: Translate to whichever letters
                                            //: best show this locale's script
                                            //: — a Latin "Aa" says nothing
                                            //: about legibility in Cyrillic,
                                            //: Greek or CJK.
                                            text: qsTr("Aa")
                                            color: root.auditPalette[
                                                problemRow.modelData.fg]
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
                                            font.weight: AppTheme.weightStrong
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            id: problemText
                                            objectName: "themeReadabilityRowLabel"
                                            Layout.fillWidth: true
                                            // Pinned to two lines so the ratio sits at
                                            // the same height in every row.
                                            Layout.preferredHeight:
                                                problemColumn.rowTextHeight
                                            verticalAlignment: Text.AlignVCenter
                                            wrapMode: Text.WordWrap
                                            maximumLineCount: 2
                                            textFormat: Text.PlainText
                                            text: problemRow.modelData.label
                                            color: AppTheme.editorText
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            textFormat: Text.PlainText
                                            text: problemRow.modelData.kind === "ink"
                                                  ? qsTr("%1:1 — needs %2:1")
                                                    .arg(problemRow.modelData.value.toFixed(1))
                                                    .arg(problemRow.modelData.minimum.toFixed(1))
                                                  : qsTr("ΔL* %1 — needs %2")
                                                    .arg(problemRow.modelData.value.toFixed(1))
                                                    .arg(problemRow.modelData.minimum.toFixed(1))
                                            color: AppTheme.editorDanger
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.textMeta
                                        }
                                    }
                                }

                                MouseArea {
                                    id: problemHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.beginEdit(
                                        problemRow.modelData.role,
                                        root.labelForRole(
                                            problemRow.modelData.role))
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true; visible: root.readabilityProblems === 0 }
            }

            Item { Layout.fillHeight: true; visible: root.editingRole.length > 0 }
        }
    }

    }

    // The base theme's own colours, offered in the picker for reuse. Sorted by
    // CIE L* rather than key order, so the strip reads as a ladder instead of a
    // run of indistinguishable near-blacks.
    readonly property var paletteSwatches: {
        var pal = root.basePalette
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
        out.sort(function (a, b) {
            return root.store.lightness(a) - root.store.lightness(b)
        })
        return out
    }
}
