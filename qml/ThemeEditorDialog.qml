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
    // Escape peels one layer at a time (see dismissTransient): while anything
    // transient is open it is handled by the Escape shortcut below, and only
    // a clean editor closes on it. Otherwise Escape in the hex field threw
    // away the whole editor.
    closePolicy: root.hasTransientState ? Popup.NoAutoClose
                                        : Popup.CloseOnEscape

    // Create the theme on open so there is something to edit; a theme with no
    // overrides simply follows its base. A host that opened the editor for one
    // job ("Add a gradient" in Settings) names the role to open with.
    property string initialRole: ""
    onOpened: {
        if (!root.store.exists)
            root.store.createTheme("")
        if (root.initialRole.length > 0) {
            root.beginEdit(root.initialRole, root.labelForRole(root.initialRole))
            // The row's own reveal ran before the list was laid out and
            // scrolled it off the top; the role it opens with is in the first
            // group, so the top of the list shows it.
            Qt.callLater(function() { roleScroll.contentY = 0 })
        }
    }

    readonly property var store: app.customTheme

    // Something Escape should close before the editor itself.
    readonly property bool hasTransientState:
        root.confirmingReset || root.confirmingDelete
        || root.editingRole.length > 0
        || (root.compact && root.reportOpen) || root.importing

    function dismissTransient() {
        if (root.confirmingReset)
            root.confirmingReset = false
        else if (root.confirmingDelete)
            root.confirmingDelete = false
        else if (root.editingRole.length > 0)
            root.editingRole = ""
        else if (root.reportOpen)
            root.reportOpen = false
        else if (root.importing) {
            root.importing = false
            root.importError = ""
        }
    }

    // ---- undo ----
    // The history lives in the store (one step per drag, reset or base
    // change); these keep the open picker in step with it.
    function undoEdit() {
        if (root.store.undo())
            root.afterHistoryMove()
    }
    function redoEdit() {
        if (root.store.redo())
            root.afterHistoryMove()
    }
    function afterHistoryMove() {
        root.confirmingReset = false
        // Undo moves colours, not gradients: the picker goes back to the
        // role's own colour.
        root.editingStop = -1
        if (root.editingRole.length > 0)
            picker.show(root.effectiveColor(root.editingRole))
    }

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

    // Graded at the WORST stop of every gradient the preview paints (the
    // theme's own and Depth's); with none it is exactly audit().
    readonly property var readabilityReport:
        root.store.auditWithGradients(root.auditPalette, root.previewGradients)
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
    // The gradient colour (stop index) the picker is changing instead of the
    // role's own colour, or -1. Chosen in GradientEditor.
    property int editingStop: -1
    readonly property bool editingTakesGradient:
        root.editingRole.length > 0 && root.store.isGradientRole(root.editingRole)
    // The stored gradients, read once per change (a QVariantMap by value).
    readonly property var storeGradients: root.store.gradients
    readonly property int gradientCount:
        root.storeGradients ? Object.keys(root.storeGradients).length : 0
    // The role under the pointer (or the keyboard) in the list, outlined in the
    // preview so it can be traced to where it paints.
    property string hoverRole: ""
    property bool confirmingReset: false
    property bool confirmingDelete: false
    // The collection's actions (New, Duplicate, Share, Import, Delete).
    property bool manageThemes: false
    onManageThemesChanged: if (!manageThemes) confirmingDelete = false
    onEditingRoleChanged: if (editingRole.length === 0) spotRoles = []
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

    // The surface gradients the preview paints: the theme's own, and, while
    // Settings -> Appearance -> Depth is on, Depth's for the grounds the theme
    // leaves flat (the same C++ stops the app draws).
    readonly property var previewGradients: {
        var out = {}
        var own = root.store.gradients
        if (own) {
            for (var k in own)
                out[k] = own[k]
        }
        if (AppTheme.surfaceDepth === 1 && typeof app !== "undefined" && app
                && app.backdrops) {
            var pal = root.previewPalette
            var dark = AppTheme.relativeLuminance(
                           AppTheme._asColor(pal.background)) < 0.18
            var grounds = ["background", "sidebar", "rail"]
            for (var i = 0; i < grounds.length; ++i) {
                var r = grounds[i]
                if (out[r] || pal[r] === undefined)
                    continue
                var stops = app.backdrops.depthStops(AppTheme._asColor(pal[r]), dark)
                if (stops && stops.length >= 2)
                    out[r] = { type: "linear", angle: 180, stops: stops }
            }
        }
        return out
    }

    // The two hot inputs, read once per change instead of per row: store.colors
    // returns a QVariantMap by value (a fresh conversion per read), and
    // paletteForTheme() builds a fresh object per call. The role list
    // instantiates every role row and repaints on every drag sample.
    readonly property var overrideColors: root.store.colors
    readonly property var basePalette:
        AppTheme.paletteForTheme(root.store.baseTheme)

    function effectiveColor(rolekey) {
        var overrides = root.overrideColors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        // paletteForTheme uses semantic names; a few store keys differ (inputBg
        // -> inputBackground, mention -> mentionBadge, reaction ->
        // reactionBackground).
        var alias = root.storeKeyAliases[rolekey]
        var lookup = alias !== undefined ? alias : rolekey
        // The resolved custom palette first: an unset role that follows an
        // edited one (Open room row after Selection) paints the EDITED colour,
        // and reading the base alone showed the stale inherited one.
        var resolved = root.previewPalette
        if (resolved && resolved[lookup] !== undefined)
            return resolved[lookup]
        var pal = root.basePalette
        if (pal[lookup] !== undefined)
            return pal[lookup]
        return AppTheme.editorTextMuted
    }

    // ---- links between roles ----
    // The base preset as stored, before paletteForTheme() fills fallbacks: a
    // role it does not set is one that follows its parent.
    readonly property var rawBasePalette:
        AppTheme.rawPaletteForTheme(root.store.baseTheme)

    // role -> parent, for every role that is unset AND takes its parent's
    // colour right now, so editing the parent recolours it. The declared
    // shape comes from the store (`follows`); a preset that sets the child
    // itself, or a user who did, breaks the link, and the colour check keeps
    // the claim honest for anything the declaration cannot see.
    readonly property var liveLinks: {
        var out = {}
        var list = root.store.roles
        var overrides = root.overrideColors
        var raw = root.rawBasePalette
        for (var i = 0; i < list.length; ++i) {
            var key = list[i].key
            var parentKey = list[i].follows
            if (!parentKey || parentKey.length === 0)
                continue
            if (overrides && overrides[key] !== undefined)
                continue
            if (raw && raw[key] !== undefined)
                continue
            if (!Qt.colorEqual(root.effectiveColor(key),
                               root.effectiveColor(parentKey)))
                continue
            out[key] = parentKey
        }
        return out
    }

    // Every role that would change with `key`, directly or through a chain
    // (Selection -> Soft accent -> Your reaction pill), in list order.
    function dependentsOf(key) {
        var out = []
        if (!key || key.length === 0)
            return out
        var links = root.liveLinks
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            var p = links[list[i].key]
            for (var hops = 0; p !== undefined && hops < 8; ++hops) {
                if (p === key) {
                    out.push(list[i].key)
                    break
                }
                p = links[p]
            }
        }
        return out
    }

    // The role the preview is tracing: the one pointed at in the list, else
    // the one open in the picker.
    readonly property string focusRole:
        root.hoverRole.length > 0 ? root.hoverRole : root.editingRole
    readonly property var focusDependents: root.dependentsOf(root.focusRole)
    readonly property var linkedRoleSet: {
        var out = {}
        var deps = root.focusDependents
        for (var i = 0; i < deps.length; ++i)
            out[deps[i]] = true
        return out
    }
    readonly property var editingDependents: root.dependentsOf(root.editingRole)
    // The open role's declared parent when the base leaves it unset, so a
    // reset makes it follow that parent again ("" otherwise).
    readonly property string editingFollowsDeclared: {
        var key = root.editingRole
        if (key.length === 0)
            return ""
        var raw = root.rawBasePalette
        if (raw && raw[key] !== undefined)
            return ""
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return list[i].follows ? list[i].follows : ""
        }
        return ""
    }
    // What the open role is currently following, or "".
    readonly property string editingParent: {
        var p = root.liveLinks[root.editingRole]
        return p !== undefined ? p : ""
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
        // A new role is a new undo step, even if the last drag never ended.
        root.store.sealUndoStep()
        // The "under the pointer" chips stay while hopping between them.
        if (root.spotRoles.indexOf(key) < 0)
            root.spotRoles = []
        root.ownColoursAtOpen = root.overrideColors
        root.editingStop = -1
        root.editingRole = key
        root.editingLabel = label
        // The panel holds one thing at a time; opening a colour takes it.
        root.reportOpen = false
        picker.load(root.effectiveColor(key))
    }

    // A click in the preview: open the role, and keep every role painted
    // under the pointer so the surface behind a label can be reached too.
    function openFromPreview(role, stack) {
        root.spotRoles = stack && stack.length > 1 ? stack : []
        root.beginEdit(role, root.labelForRole(role))
    }

    // Roles under the last preview click, the clicked one first.
    property var spotRoles: []
    // This theme's own colours when the open role was opened (see
    // paletteSwatches).
    property var ownColoursAtOpen: ({})

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

    readonly property string gradientWord: qsTr("gradient").toLowerCase()

    // Groups narrowed by the filter; empty groups are dropped.
    readonly property var roleGroups: {
        var out = []
        var seen = {}
        var needle = root.roleFilter.trim().toLowerCase()
        // "#283097" (or "283097") finds every role painted that colour: the
        // reverse lookup. Only a hex-looking needle reads the colours, so
        // typing a word does not rebuild the list on every drag sample.
        var hexNeedle = /^#?[0-9a-f]{3,6}$/.test(needle)
                        ? (needle.charAt(0) === "#" ? needle : "#" + needle)
                        : ""
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            var hit = needle.length === 0
                || list[i].label.toLowerCase().indexOf(needle) >= 0
                || list[i].group.toLowerCase().indexOf(needle) >= 0
                || list[i].hint.toLowerCase().indexOf(needle) >= 0
                || list[i].key.toLowerCase().indexOf(needle) >= 0
            // "grad…" lists the surfaces that can be a gradient.
            if (!hit && needle.length >= 3
                    && root.gradientWord.indexOf(needle) === 0
                    && root.store.isGradientRole(list[i].key))
                hit = true
            if (!hit && hexNeedle.length > 0) {
                var hex = root.toHex(Qt.color(String(
                              root.effectiveColor(list[i].key))))
                hit = hex.toLowerCase().indexOf(hexNeedle) === 0
            }
            if (!hit)
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

    // A role as a small clickable chip (its colour and name) that opens it:
    // the "under the pointer" list and the follows/followed-by links.
    component RoleChip: Rectangle {
        id: chip
        required property string roleKey
        readonly property bool current: root.editingRole === chip.roleKey
        objectName: "themeRoleChip_" + chip.roleKey
        implicitWidth: chipRow.implicitWidth + AppTheme.spacing8 * 2
        implicitHeight: 24
        radius: AppTheme.radiusPill
        color: chip.current ? AppTheme.editorSelection
             : chipHover.containsMouse ? AppTheme.editorInset : "transparent"
        border.width: 1
        border.color: chip.current ? AppTheme.editorAccent
                                   : AppTheme.editorBorderStrong
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: root.labelForRole(chip.roleKey)
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                    || e.key === Qt.Key_Space) {
                root.beginEdit(chip.roleKey, root.labelForRole(chip.roleKey))
                e.accepted = true
            }
        }
        // Pointing at a chip traces the role in the preview, as a list row
        // does.
        readonly property bool pointed: chipHover.containsMouse || activeFocus
        onPointedChanged: {
            if (pointed)
                root.hoverRole = chip.roleKey
            else if (root.hoverRole === chip.roleKey)
                root.hoverRole = ""
        }
        Component.onDestruction: {
            if (root.hoverRole === chip.roleKey)
                root.hoverRole = ""
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            visible: chip.activeFocus
            radius: AppTheme.radiusPill
            color: "transparent"
            border.width: 2
            border.color: AppTheme.editorAccent
        }
        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: AppTheme.spacing6
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 10
                height: 10
                radius: 5
                color: root.effectiveColor(chip.roleKey)
                border.width: 1
                border.color: AppTheme.editorBorderStrong
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.PlainText
                text: root.labelForRole(chip.roleKey)
                color: AppTheme.editorText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
        }
        MouseArea {
            id: chipHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.beginEdit(chip.roleKey,
                                      root.labelForRole(chip.roleKey))
        }
    }

    background: Rectangle {
        color: AppTheme.editorCanvas
    }

    // A plain Item: the picker/report panel is positioned, not laid out (it
    // moves column in compact mode), and a child of a ColumnLayout would become
    // a layout item.
    contentItem: Item {

    // Inside the popup, so the modal editor's own shortcuts are the live
    // ones (Qt blocks shortcuts outside a modal popup, including Settings'
    // Escape). A focused text field keeps Ctrl+Z for its own text.
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: root.opened && root.store.canUndo
        onActivated: root.undoEdit()
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: root.opened && root.store.canRedo
        onActivated: root.redoEdit()
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.opened && root.hasTransientState
        onActivated: root.dismissTransient()
    }

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
                            objectName: "themeEditorSubtitle"
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            // Said first when it matters: the app keeps its
                            // current theme until "Use this theme".
                            readonly property bool previewOnly:
                                app.settings.theme !== 12
                                && (root.store.overrideCount > 0
                                    || root.gradientCount > 0)
                            text: previewOnly
                                  ? qsTr("Preview only: choose Use this theme to see it everywhere.")
                                  : root.store.overrideCount === 0
                                    ? qsTr("Click any part of the sample window, or a role on the left.")
                                    : qsTr("%n colour(s) changed.",
                                           "custom theme, count of edited roles",
                                           root.store.overrideCount)
                            color: previewOnly ? AppTheme.editorText
                                               : AppTheme.editorTextMuted
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

                    // Undo/redo of colour edits (also Ctrl+Z and
                    // Ctrl+Shift+Z). Hidden while the reset question is up,
                    // which they would answer behind its back.
                    EditorButton {
                        objectName: "themeUndoButton"
                        visible: !root.confirmingReset
                        enabled: root.store.canUndo
                        text: qsTr("Undo")
                        Accessible.description: qsTr("Undo the last colour change")
                        onClicked: root.undoEdit()
                    }
                    EditorButton {
                        objectName: "themeRedoButton"
                        visible: !root.confirmingReset
                        enabled: root.store.canRedo
                        text: qsTr("Redo")
                        Accessible.description: qsTr("Redo the colour change you undid")
                        onClicked: root.redoEdit()
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !root.confirmingReset
                        width: 1
                        height: 20
                        color: AppTheme.editorBorder
                    }

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

                    // Your themes. The collection's actions sit behind Manage:
                    // open, they took about 120px from the role list on every
                    // visit for buttons used once per theme (at 1366x768 the
                    // list showed eight roles).
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Your themes")
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                            elide: Label.ElideRight
                        }
                        EditorButton {
                            objectName: "themeManageToggleButton"
                            implicitHeight: 24
                            text: root.manageThemes ? qsTr("Done")
                                                    : qsTr("Manage")
                            Accessible.description:
                                qsTr("New, duplicate, share, import or delete themes")
                            onClicked: root.manageThemes = !root.manageThemes
                        }
                    }

                    // Shown whenever there is a choice to make; one theme
                    // needs no switcher.
                    Flow {
                        Layout.fillWidth: true
                        visible: root.manageThemes
                                 || root.store.themes.length > 1
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
                        visible: root.manageThemes && !root.confirmingDelete
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
                            // Asks first: a deleted theme cannot be undone,
                            // and its history goes with it.
                            onClicked: root.confirmingDelete = true
                        }
                    }

                    // Delete, confirmed inline (the reset question's pattern).
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.confirmingDelete
                        spacing: AppTheme.spacing6
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            text: qsTr("Delete “%1”? This cannot be undone.")
                                  .arg(root.store.name.length > 0
                                       ? root.store.name : qsTr("Untitled"))
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Row {
                            spacing: AppTheme.spacing6
                            EditorButton {
                                objectName: "customThemeDeleteConfirmButton"
                                danger: true
                                text: qsTr("Delete")
                                onClicked: {
                                    root.confirmingDelete = false
                                    root.store.deleteTheme(root.store.activeThemeId)
                                    root.editingRole = ""
                                }
                            }
                            EditorButton {
                                text: qsTr("Keep")
                                onClicked: root.confirmingDelete = false
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
                    // the role list. One row: the label and the current base,
                    // which is itself the toggle (two rows cost a role's worth
                    // of height on a small window).
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
                        Rectangle {
                            id: baseToggle
                            objectName: "themeBasesToggleButton"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: 32
                            radius: AppTheme.radiusMd
                            color: baseToggleHover.containsMouse
                                   ? AppTheme.editorSelection
                                   : AppTheme.editorInset
                            border.width: 1
                            border.color: root.basesExpanded
                                          ? AppTheme.editorAccent
                                          : AppTheme.editorBorder
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Base theme: %1. Change")
                                             .arg(root.baseThemeName)
                            Keys.onPressed: (e) => {
                                if (e.key === Qt.Key_Return
                                    || e.key === Qt.Key_Enter
                                    || e.key === Qt.Key_Space) {
                                    root.basesExpanded = !root.basesExpanded
                                    e.accepted = true
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                visible: baseToggle.activeFocus
                                radius: AppTheme.radiusSm
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.editorAccent
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacing8
                                anchors.rightMargin: AppTheme.spacing6
                                spacing: AppTheme.spacing8
                                Row {
                                    spacing: 1
                                    Repeater {
                                        model: ["sidebar", "background", "accent"]
                                        delegate: Rectangle {
                                            required property string modelData
                                            width: 8
                                            height: 18
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
                                Icon {
                                    name: root.basesExpanded ? "expand_less"
                                                             : "expand_more"
                                    size: 16
                                    color: AppTheme.editorTextSecondary
                                }
                            }
                            MouseArea {
                                id: baseToggleHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.basesExpanded = !root.basesExpanded
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

                    // Gradients belong to six large surfaces and were found
                    // only by opening one of them and reading below its
                    // picker. One button opens the largest, where the Fill
                    // choice is the first control.
                    EditorButton {
                        objectName: "themeMakeGradientButton"
                        Layout.fillWidth: true
                        text: qsTr("Make a gradient")
                        Accessible.description: qsTr("Opens the conversation "
                            + "background with its fill choices: a solid colour "
                            + "or a gradient")
                        onClicked: root.beginEdit("background",
                                                  root.labelForRole("background"))
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

                        // The custom handle has no fade of its own, so
                        // AsNeeded drew a full-height bar over a filtered
                        // list that fits; show it only when it scrolls.
                        ScrollBar.vertical: ScrollBar {
                            policy: roleScroll.contentHeight > roleScroll.height + 1
                                    ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
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
                                            // The role's gradient, or null: painted
                                            // in the swatch and named on the row,
                                            // so a gradient is visible in the list.
                                            readonly property var gradientSpec: {
                                                var g = root.storeGradients
                                                return g && g[modelData.key]
                                                       ? g[modelData.key] : null
                                            }
                                            // The role this one is riding on
                                            // right now ("" if it has its own
                                            // colour), named on the row.
                                            readonly property string followsKey: {
                                                var p = root.liveLinks[modelData.key]
                                                return p !== undefined ? p : ""
                                            }
                                            Layout.fillWidth: true
                                            implicitHeight: 38
                                            radius: AppTheme.radiusSm
                                            color: editing ? AppTheme.editorSelection
                                                 : roleHover.containsMouse
                                                   ? AppTheme.editorInset
                                                   : "transparent"

                                            // Where this role paints, shown in the
                                            // preview while the row is pointed at or
                                            // focused.
                                            readonly property bool pointed:
                                                roleHover.containsMouse || activeFocus
                                            onPointedChanged: {
                                                if (pointed)
                                                    root.hoverRole = modelData.key
                                                else if (root.hoverRole === modelData.key)
                                                    root.hoverRole = ""
                                            }
                                            Component.onDestruction: {
                                                if (root.hoverRole === modelData.key)
                                                    root.hoverRole = ""
                                            }

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
                                            function revealInList() {
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
                                            onActiveFocusChanged:
                                                if (activeFocus) revealInList()
                                            // A role opened from the preview is
                                            // shown in the list too, so a click
                                            // there says which row it was.
                                            onEditingChanged:
                                                if (editing) revealInList()

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
                                                        ThemedSurface {
                                                            objectName: "themeSwatchGradient_"
                                                                + roleRow.modelData.key
                                                            anchors.fill: parent
                                                            anchors.margins: 2
                                                            visible: roleRow.gradientSpec !== null
                                                            role: roleRow.modelData.key
                                                            flatFill: false
                                                            specOverride: roleRow.gradientSpec
                                                        }
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
                                                    // The hex in mono, then what
                                                    // the role follows in the UI
                                                    // face: one mono run read as
                                                    // code and elided the parent's
                                                    // name ("follows Conversation
                                                    // ba…").
                                                    RowLayout {
                                                        Layout.fillWidth: true
                                                        spacing: AppTheme.spacing6
                                                        Label {
                                                            objectName: "themeRoleHex_"
                                                                + roleRow.modelData.key
                                                            textFormat: Text.PlainText
                                                            // A follower's parent row
                                                            // already says when it is
                                                            // see-through.
                                                            text: roleRow.gradientSpec !== null
                                                                  ? (roleRow.gradientSpec.type === "radial"
                                                                     ? qsTr("Radial gradient")
                                                                     : qsTr("Linear gradient"))
                                                                  : roleRow.seeThrough
                                                                    && roleRow.followsKey.length === 0
                                                                    ? qsTr("%1 · see-through")
                                                                      .arg(roleRow.hex)
                                                                    : roleRow.hex
                                                            color: AppTheme.editorTextMuted
                                                            font.family: roleRow.gradientSpec !== null
                                                                         ? AppTheme.uiFont
                                                                         : AppTheme.monoFont
                                                            font.pixelSize: AppTheme.menuSectionSize
                                                        }
                                                        // Editing the parent
                                                        // recolours this one.
                                                        Label {
                                                            objectName: "themeRoleFollows_"
                                                                + roleRow.modelData.key
                                                            visible: roleRow.followsKey.length > 0
                                                            Layout.fillWidth: true
                                                            Layout.minimumWidth: 0
                                                            textFormat: Text.PlainText
                                                            text: qsTr("follows %1")
                                                                  .arg(root.labelForRole(
                                                                      roleRow.followsKey))
                                                            color: AppTheme.editorTextMuted
                                                            font.family: AppTheme.uiFont
                                                            font.pixelSize: AppTheme.menuSectionSize
                                                            elide: Label.ElideRight
                                                        }
                                                        Item {
                                                            visible: roleRow.followsKey.length === 0
                                                            Layout.fillWidth: true
                                                        }
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

                        // Rendered at natural size and scaled uniformly to fit:
                        // stretching would reflow the mock into proportions the
                        // real window never has. Scaling UP is allowed (to
                        // 1.6x): at 1920x1080 the natural 880x560 used about
                        // 40% of the stage and left hairlines and presence dots
                        // too small to click. The preview's text is distance-
                        // field rendered (only Icon uses NativeRendering, and
                        // the preview draws no Icon), so it stays sharp.
                        ThemePreviewDemo {
                            id: preview
                            objectName: "themePreviewDemo"
                            pal: root.previewPalette
                            // The user's own layout, so colours land where they
                            // will actually be seen.
                            channels: app.settings
                                      && app.settings.roomNavigationLayout === 1
                            highlightRole: root.editingRole
                            hoverRole: root.hoverRole
                            // Surface gradients (the theme's own and Depth's).
                            gradients: root.previewGradients
                            // Everything that changes with the role being
                            // traced, outlined lightly.
                            linkedRoles: root.linkedRoleSet
                            // 2px on screen at any scale.
                            outlineWidth: 2 / Math.max(0.25, scale)
                            width: implicitWidth
                            height: implicitHeight
                            transformOrigin: Item.TopLeft
                            scale: Math.min(1.6,
                                            previewStage.width / implicitWidth,
                                            previewStage.height / implicitHeight)
                            x: (previewStage.width - implicitWidth * scale) / 2
                            y: (previewStage.height - implicitHeight * scale) / 2
                            onRegionActivated: (role, stack) =>
                                root.openFromPreview(role, stack)
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

            // Everything about the open role, scrollable: on a 768px window
            // the picker, its links and the readability rows are taller than
            // the panel. The picker's own drags never scroll it (they set
            // preventStealing).
            Flickable {
                id: editScroll
                objectName: "themeEditScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.editingRole.length > 0
                clip: true
                contentWidth: width
                contentHeight: editColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                // Content taller than the panel. Only then is there a bar,
                // and only then does the column give up a gutter for it: a
                // bar drawn over the content covered the hex field's edge
                // and the last digit of every readability target.
                readonly property bool overflows: contentHeight > height + 1
                interactive: overflows

                ScrollBar.vertical: ScrollBar {
                    policy: editScroll.overflows ? ScrollBar.AlwaysOn
                                                 : ScrollBar.AlwaysOff
                    contentItem: Rectangle {
                        implicitWidth: 5
                        radius: 2.5
                        color: AppTheme.editorBorderStrong
                    }
                }

                ColumnLayout {
                    id: editColumn
                    width: editScroll.width
                           - (editScroll.overflows ? AppTheme.spacing12 : 0)
                    spacing: AppTheme.spacing12

                    // Every role painted where the preview was clicked, so the
                    // fill behind a label is one click away (clicking the Send
                    // button lands on its label).
                    ColumnLayout {
                        objectName: "themeSpotRoles"
                        Layout.fillWidth: true
                        visible: root.spotRoles.length > 1
                        spacing: AppTheme.spacing4
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Under the pointer")
                            color: AppTheme.editorTextMuted
                            font.family: AppTheme.menuSectionFont
                            font.pixelSize: AppTheme.menuSectionSize
                            font.weight: AppTheme.menuSectionWeight
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: root.spotRoles
                                delegate: RoleChip {
                                    required property string modelData
                                    roleKey: modelData
                                }
                            }
                        }
                    }

                    ColorPickerPanel {
                        id: picker
                        objectName: "themeColorPicker"
                        Layout.fillWidth: true
                        visible: root.editingRole.length > 0
                        title: root.editingLabel
                        subtitle: root.hintForRole(root.editingRole)
                        // The reset button resets the role's own colour, so it
                        // is not offered while the picker is on a gradient
                        // colour.
                        canReset: root.editingRole.length > 0
                                  && root.editingStop < 0
                                  && root.isOverridden(root.editingRole)
                        // A role with a parent goes back to following it.
                        resetLabel: root.editingFollowsDeclared.length > 0
                                    ? qsTr("Reset: follow %1 again")
                                      .arg(root.labelForRole(
                                          root.editingFollowsDeclared))
                                    : qsTr("Reset to the base theme")
                        suggestions: root.paletteSwatches
                        // Surface roles: the Fill choice (solid or gradient)
                        // first, then the picker for whichever colour is
                        // being changed.
                        accessoryVisible: root.editingTakesGradient
                        bodyCaption: {
                            if (!root.editingTakesGradient || !gradientEditor.hasGradient)
                                return ""
                            if (root.editingStop >= 0)
                                return qsTr("Gradient colour: %1")
                                       .arg(gradientEditor.stopName(root.editingStop))
                            return qsTr("Solid colour underneath the gradient, "
                                        + "also used by what follows this one")
                        }
                        accessory: GradientEditor {
                            id: gradientEditor
                            objectName: "themeGradientEditor"
                            Layout.fillWidth: true
                            role: root.editingTakesGradient ? root.editingRole
                                                            : "background"
                            baseColor: root.editingRole.length > 0
                                       ? root.effectiveColor(root.editingRole)
                                       : AppTheme.editorCanvas
                            gradedPalette: root.auditPalette
                            selectedStop: root.editingStop
                            onStopSelected: (index) => {
                                root.editingStop = index
                                picker.load(index >= 0
                                            ? gradientEditor.stopAt(index)
                                            : root.effectiveColor(root.editingRole))
                            }
                        }
                        onPicked: (value) => {
                            if (root.editingRole.length === 0)
                                return
                            if (root.editingStop >= 0 && gradientEditor.hasGradient)
                                gradientEditor.setStop(root.editingStop,
                                                       root.toHex(value))
                            else
                                root.store.setColor(root.editingRole,
                                                    root.toHex(value))
                        }
                        // One drag, swatch or typed value is one undo step.
                        onGestureFinished: root.store.sealUndoStep()
                        onResetRequested: {
                            if (root.editingRole.length > 0) {
                                root.store.resetColor(root.editingRole)
                                // show(), not load(): the "before" half keeps
                                // the colour the role had when it was opened.
                                picker.show(root.effectiveColor(root.editingRole))
                            }
                        }
                        onClosed: root.editingRole = ""
                    }

                    // How the open role is tied to others: what it follows,
                    // and what follows it. Editing a parent silently
                    // recoloured up to six other places before this said so.
                    ColumnLayout {
                        objectName: "themeRoleLinks"
                        Layout.fillWidth: true
                        visible: root.editingParent.length > 0
                                 || root.editingDependents.length > 0
                        spacing: AppTheme.spacing4

                        Label {
                            objectName: "themeRoleFollowsNote"
                            Layout.fillWidth: true
                            visible: root.editingParent.length > 0
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            text: qsTr("Follows %1 until you change it here.")
                                  .arg(root.labelForRole(root.editingParent))
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Flow {
                            Layout.fillWidth: true
                            visible: root.editingParent.length > 0
                            spacing: AppTheme.spacing4
                            RoleChip { roleKey: root.editingParent }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: root.editingParent.length > 0
                                              ? AppTheme.spacing6 : 0
                            visible: root.editingDependents.length > 0
                            wrapMode: Text.WordWrap
                            // No count: the English catalog has no numerus
                            // forms, so "%n place(s)" showed literally.
                            text: qsTr("Changing this also recolours what follows it:")
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Flow {
                            objectName: "themeRoleDependents"
                            Layout.fillWidth: true
                            visible: root.editingDependents.length > 0
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: root.editingDependents
                                delegate: RoleChip {
                                    required property string modelData
                                    roleKey: modelData
                                }
                            }
                        }
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

                // Where a newcomer looks first: gradients exist, and which
                // surfaces take one, each a click away. Not shown over a list
                // of readability problems, which matter more.
                ColumnLayout {
                    objectName: "themeGradientTip"
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacing8
                    visible: root.readabilityProblems === 0
                    spacing: AppTheme.spacing6
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Gradients")
                        color: AppTheme.editorTextMuted
                        font.family: AppTheme.menuSectionFont
                        font.pixelSize: AppTheme.menuSectionSize
                        font.weight: AppTheme.menuSectionWeight
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: qsTr("The large areas can be a gradient instead of "
                                   + "one colour. Open one, then choose Linear or "
                                   + "Radial under Fill, or start from a preset.")
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing4
                        Repeater {
                            model: ["background", "sidebar", "rail", "surface"]
                                   .filter(function(k) {
                                       return root.store.isGradientRole(k)
                                   })
                            delegate: RoleChip {
                                required property string modelData
                                roleKey: modelData
                            }
                        }
                    }
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

                    // As the role list: a bar only when the list scrolls.
                    ScrollBar.vertical: ScrollBar {
                        policy: problemScroll.contentHeight > problemScroll.height + 1
                                ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
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

        }
    }

    }

    // The base theme's own colours, and every colour this theme has set, offered
    // in the picker for reuse (the user's own choices are the ones most worth
    // repeating across roles). Sorted by CIE L* rather than key order, so the
    // strip reads as a ladder instead of a run of indistinguishable
    // near-blacks.
    readonly property var paletteSwatches: {
        var pal = root.basePalette
        var keys = ["background", "sidebar", "rail", "surface", "cardElevated",
                    "hover", "selected", "border", "borderStrong",
                    "inputBackground", "accent", "link", "textPrimary",
                    "textSecondary", "textMuted", "ownBubble", "otherBubble"]
        var out = []
        var seen = {}
        // A snapshot taken when a role opens: read live, the strip would be
        // rebuilt, and the dragged colour would wander along the ladder, on
        // every sample of a drag.
        var overrides = root.ownColoursAtOpen
        if (overrides) {
            for (var role in overrides) {
                if (role === root.editingRole)
                    continue
                var own = String(overrides[role]).toUpperCase()
                if (!/^#[0-9A-F]{6}$/.test(own) || seen[own] !== undefined)
                    continue
                seen[own] = true
                out.push(own)
            }
        }
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
