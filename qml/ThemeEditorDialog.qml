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
    // Escape peels one layer at a time (peelEscape): a transient layer, then
    // the role filter, then the editor itself. All of it goes through the one
    // Escape Shortcut below, never the close policy: CloseOnEscape needs the
    // popup to HAVE active focus, which it loses once a transient field is
    // destroyed or the empty canvas is clicked, so a clean editor ignored
    // Escape (live 2026-10-07, three tries). Escape in the hex field still
    // closes only the picker.
    closePolicy: Popup.NoAutoClose

    // Create the theme on open so there is something to edit; a theme with no
    // overrides simply follows its base. A host that opened the editor for one
    // job ("Add a gradient" in Settings) names the role to open with.
    property string initialRole: ""
    // The theme this open created and its untouched shape, so closing without
    // an edit or "Use this theme" leaves nothing stored behind.
    property string createdId: ""
    property string createdName: ""
    property int createdBase: -1
    function discardUntouchedCreation() {
        var id = root.createdId
        root.createdId = ""
        if (id.length === 0 || root.store.activeThemeId !== id)
            return
        if (root.store.themes.length !== 1 || app.settings.theme === 12
                || root.store.overrideCount > 0 || root.gradientCount > 0
                || root.store.name !== root.createdName
                || root.store.baseTheme !== root.createdBase)
            return
        root.store.deleteTheme(id)
    }
    onClosed: root.discardUntouchedCreation()
    onOpened: {
        root.createdId = ""
        if (!root.store.exists) {
            root.createdId = root.store.createTheme("")
            root.createdName = root.store.name
            root.createdBase = root.store.baseTheme
        }
        // A theme with nothing changed starts at Simple; one that has been
        // worked on starts where its changes are. A level the user has picked
        // is kept for as long as the editor lives.
        if (!root.levelChosen)
            root.level = root.store.overrideCount > 0 || root.gradientCount > 0
                         ? 2 : 0
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
        || root.reportOpen || root.menuOpen || root.shareOpen
        || root.pendingBase >= 0

    function peelEscape() {
        if (root.hasTransientState) {
            root.dismissTransient()
        } else if (filterField.text.length > 0 || root.roleFilter.length > 0) {
            filterField.text = ""
            root.roleFilter = ""
        } else {
            root.close()
        }
    }

    function dismissTransient() {
        // The topmost layer first: a popover, then the preset question.
        if (root.reportOpen)
            root.reportOpen = false
        else if (root.shareOpen)
            root.shareOpen = false
        else if (root.menuOpen)
            root.menuOpen = false
        else if (root.pendingBase >= 0)
            root.pendingBase = -1
        else if (root.confirmingReset)
            root.confirmingReset = false
        else if (root.confirmingDelete)
            root.confirmingDelete = false
        else if (root.editingRole.length > 0)
            root.editingRole = ""
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
        // Undo moves colours, not gradients: a role with a gradient stays on
        // the gradient colour it was editing (the one that is painted), any
        // other goes back to its own colour.
        if (root.editingRole.length > 0) {
            root.editingStop = root.startingStop(root.editingRole, root.editingStop)
            picker.show(root.pickerColourFor(root.editingRole, root.editingStop))
        } else {
            root.editingStop = -1
        }
    }

    // The gradient stops of a role, or null.
    function gradientStopsOf(key) {
        var all = root.storeGradients
        var g = all && root.store.isGradientRole(key) ? all[key] : undefined
        return g && g.stops && g.stops.length >= 2 ? g.stops : null
    }
    // The stop the picker should be on for `key`: a role with a gradient is
    // painted by its stops, and the flat colour underneath is invisible, so
    // opening the picker on that colour made an edit do nothing on screen.
    // `wanted` is kept when it is a real stop of the role's gradient.
    function startingStop(key, wanted) {
        var stops = root.gradientStopsOf(key)
        if (!stops)
            return -1
        return wanted >= 0 && wanted < stops.length ? wanted : 0
    }
    function pickerColourFor(key, stop) {
        var stops = root.gradientStopsOf(key)
        return stop >= 0 && stops && stop < stops.length
               ? stops[stop] : root.effectiveColor(key)
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
    // The readability report is a popover under the header's readability pill,
    // at every width; the right-hand panel is the inspector alone (with a
    // hint while nothing is open, in wide mode, so the preview does not
    // reflow when a colour is opened).
    property bool reportOpen: false
    readonly property bool pickerPanelVisible:
        !root.compact || root.editingRole.length > 0
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
    // The theme menu under the top bar (the list of themes, New, Duplicate,
    // Share, Import, Delete), and the Share / Import popover.
    property bool menuOpen: false
    onMenuOpenChanged: {
        if (menuOpen) {
            reportOpen = false
            shareOpen = false
        } else {
            confirmingDelete = false
        }
    }
    property bool shareOpen: false
    onShareOpenChanged: {
        if (shareOpen) {
            reportOpen = false
            menuOpen = false
        } else {
            copyNote = ""
        }
    }
    onReportOpenChanged: {
        if (reportOpen) {
            menuOpen = false
            shareOpen = false
        }
    }
    onEditingRoleChanged: {
        root.showDependents = false
        if (editingRole.length === 0) {
            spotRoles = []
            editingStop = -1
        }
    }
    // A preset asked for while the theme has edits of its own: -1 or its id,
    // waiting on the Keep / Replace / Cancel question.
    property int pendingBase: -1
    // Another theme is a different question.
    readonly property string activeId: root.store.activeThemeId
    onActiveIdChanged: root.pendingBase = -1
    function presetName(id) {
        var list = AppTheme.themeList
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return list[i].name
        }
        return ""
    }
    // Every way of choosing a preset comes through here. With nothing of the
    // user's own to lose it applies at once; otherwise it asks.
    function requestBase(id) {
        if (id === root.store.baseTheme)
            return
        if (root.store.overrideCount > 0 || root.gradientCount > 0)
            root.pendingBase = id
        else
            root.store.baseTheme = id
    }
    // Keep: the preset becomes the base and the edits stay on top of it.
    // Replace: the edits go too, as one undoable step.
    function resolvePendingBase(replace) {
        var id = root.pendingBase
        root.pendingBase = -1
        if (id < 0)
            return
        var name = root.presetName(id)
        root.store.sealUndoStep()
        if (replace) {
            root.store.beginUndoGroup(qsTr("Replace with %1").arg(name))
            root.store.resetAll()
            root.store.baseTheme = id
            root.store.endUndoGroup()
        } else {
            root.store.nameNextStep(qsTr("Start from %1").arg(name))
            root.store.baseTheme = id
        }
        if (root.editingRole.length > 0)
            root.afterHistoryMove()
        root.announce(replace
                      ? qsTr("Started over from %1.").arg(name)
                      : qsTr("Now based on %1; your changes are kept.").arg(name),
                      root.undoEdit)
    }

    // The base-theme grid, collapsed by default (see below).
    property bool basesExpanded: false
    // Filter for the role list.
    property string roleFilter: ""
    // How much structure the editor shows: 0 Simple (presets and four base
    // colours), 1 Groups (the colours that others follow), 2 Elements (every
    // part of the window). It changes what the left column lists and which
    // action the inspector leads with; a click on the sample window always
    // opens the element it landed on, at every level.
    property int level: 0
    property bool levelChosen: false
    function setLevel(n) {
        root.levelChosen = true
        root.level = Math.max(0, Math.min(2, n))
    }
    // The inspector's list of what follows the open colour is folded away
    // until asked for.
    property bool showDependents: false
    // Share / Import. `shareMode` is "share" or "import"; `importText` is the
    // field's text (a dropped file lands in it too); `copyNote` says the code
    // was copied.
    property string shareMode: "share"
    property string importText: ""
    property string copyNote: ""

    function openShare(mode) {
        root.shareMode = mode
        root.menuOpen = false
        root.shareOpen = true
    }

    // The store's own validation, plus what the editor can say about the
    // incoming theme: a small swatch and a readability count. The palette is an
    // ESTIMATE: the incoming base preset with the colours it sets laid on top,
    // and each unset element that follows a colour the theme sets taking that
    // colour. The live resolver (AppTheme.paletteForTheme) can only resolve the
    // theme the editor is editing, so this stands in for it.
    function resolveImported(baseId, colours) {
        var pal = AppTheme.paletteForTheme(baseId)
        var raw = AppTheme.rawPaletteForTheme(baseId)
        var aliases = root.storeKeyAliases
        var out = {}
        for (var k in pal)
            out[k] = pal[k]
        var keyOf = function (r) {
            var a = aliases[r]
            return a !== undefined ? a : r
        }
        var set = {}
        for (var r in colours) {
            out[keyOf(r)] = colours[r]
            set[r] = true
        }
        var list = root.store.roles
        for (var pass = 0; pass < 4; ++pass) {
            for (var i = 0; i < list.length; ++i) {
                var role = list[i].key
                var parentKey = list[i].follows
                if (!parentKey || set[role] === true || !set[parentKey]
                        || (raw && raw[role] !== undefined))
                    continue
                out[keyOf(role)] = out[keyOf(parentKey)]
                set[role] = true
            }
        }
        return out
    }
    function describeImport(payload) {
        var info = root.store.previewImport(payload)
        if (!info.ok)
            return { ok: false, error: info.error }
        var pal = root.resolveImported(info.base, info.colors)
        var swatch = {}
        var keys = ["rail", "sidebar", "background", "surface", "accent",
                    "textPrimary", "ownBubble", "otherBubble"]
        for (var i = 0; i < keys.length; ++i)
            swatch[keys[i]] = pal[keys[i]]
        return {
            ok: true,
            name: info.name,
            baseName: root.presetName(info.base),
            colourCount: info.colourCount,
            gradientCount: info.gradientCount,
            problems: root.store.auditWithGradients(pal, info.gradients).length,
            atLimit: info.atLimit,
            swatch: swatch
        }
    }
    readonly property var importPreview:
        root.shareOpen && root.shareMode === "import"
        && root.importText.trim().length > 0
        ? root.describeImport(root.importText) : null

    // "Copy code": the active theme, to the clipboard.
    function copyThemeCode() {
        var payload = root.store.exportTheme(root.store.activeThemeId)
        if (payload.length === 0)
            return
        themeClipboard.text = payload
        themeClipboard.selectAll()
        themeClipboard.copy()
        themeClipboard.text = ""
        root.copyNote = qsTr("Copied. Paste it to share it.")
    }

    // After an import landed: close the popover and report what the theme
    // holds, since nobody has checked it. Deferred a frame because the store
    // has only just emitted its change. The audit is forced first: the
    // throttle assigns auditPalette imperatively, which destroyed its binding,
    // so otherwise the count would describe the previous theme.
    function importLanded(replaced) {
        root.importText = ""
        root.shareOpen = false
        root.editingRole = ""
        root.auditPalette = root.previewPalette
        importNoticeTimer.replaced = replaced
        importNoticeTimer.restart()
    }
    function importAsNew() {
        var reason = root.store.importTheme(root.importText)
        if (reason.length > 0) {
            root.announce(reason, null)
            return
        }
        root.importLanded(false)
    }
    function replaceWithImport() {
        var reason = root.store.replaceActiveTheme(root.importText)
        if (reason.length > 0) {
            root.announce(reason, null)
            return
        }
        root.importLanded(true)
    }
    Timer {
        id: importNoticeTimer
        interval: 0
        property bool replaced: false
        onTriggered: {
            var text = root.readabilityProblems > 0
                ? qsTr("Theme imported. %n thing(s) in it will be hard to read.",
                       "custom theme readability", root.readabilityProblems)
                : qsTr("Theme imported.")
            // A replaced theme can be put back; a new one is just another theme.
            root.announce(text, importNoticeTimer.replaced ? root.undoEdit : null)
        }
    }

    // A theme file or code dropped on the editor: it goes to the Import tab
    // with its preview, never straight into the collection.
    function receiveThemeText(text) {
        if (text.length === 0) {
            root.announce(qsTr("That is not a theme file Lightning can read."), null)
            return
        }
        root.importText = text.trim()
        root.openShare("import")
    }
    function handleDrop(drop) {
        var text = ""
        if (drop.hasUrls && drop.urls.length > 0)
            text = root.store.readThemeFile(drop.urls[0])
        if (text.length === 0 && drop.hasText)
            text = String(drop.text).substring(0, 24576)
        drop.accept(Qt.CopyAction)
        root.receiveThemeText(text)
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
        // An element of a gradient group shows the group's gradient until it
        // has a colour of its own (the rule the live shell paints by).
        var overrides = root.overrideColors
        for (var el in AppTheme.gradientGroups) {
            if (out[el])
                continue
            var inherited = AppTheme.inheritedGradient(el, own, overrides)
            if (inherited)
                out[el] = inherited
        }
        if (AppTheme.surfaceDepth === 1 && typeof app !== "undefined" && app
                && app.backdrops) {
            var pal = root.previewPalette
            var dark = AppTheme.relativeLuminance(
                           AppTheme._asColor(pal.background)) < 0.18
            var grounds = AppTheme._depthRoles
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
    // The group the open role belongs to (its declared `follows`), whether or
    // not it currently takes the group's colour: "Apply to all <group>" is
    // offered on it either way.
    readonly property string editingGroup: {
        var key = root.editingRole
        if (key.length === 0)
            return ""
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return list[i].follows ? list[i].follows : ""
        }
        return ""
    }
    // True while the open role has a colour or a gradient of its own, so
    // "Reset to group" has something to drop.
    readonly property bool editingHasOwn:
        root.editingRole.length > 0
        && (root.isOverridden(root.editingRole)
            || (root.storeGradients
                && root.storeGradients[root.editingRole] !== undefined))

    // A message over the preview after an edit that does not look
    // reversible. `undoAction` (a function) adds an Undo, named after the
    // history step it takes (the step just recorded); without one the message
    // stands alone.
    property var toastUndoAction: null
    function announce(text, undoAction) {
        root.toastUndoAction = undoAction ? undoAction : null
        toast.actionHint = undoAction && root.store.undoLabel.length > 0
                           ? qsTr("Undo: %1").arg(root.store.undoLabel) : ""
        toast.show(text, undoAction ? qsTr("Undo") : "")
    }

    // The Undo / Redo tooltips: what the next step would take back or return.
    readonly property string undoTip:
        root.store.canUndo
        ? qsTr("Undo: %1").arg(root.store.undoLabel)
        : qsTr("Nothing to undo")
    readonly property string redoTip:
        root.store.canRedo
        ? qsTr("Redo: %1").arg(root.store.redoLabel)
        : qsTr("Nothing to redo")

    function hasOwnGradient(key) {
        return root.storeGradients && root.storeGradients[key] !== undefined
    }

    // "Reset to group": the open element drops its own colour and gradient
    // and shows its group's again.
    function resetToGroup() {
        var key = root.editingRole
        if (key.length === 0)
            return
        var group = root.editingGroup
        // A colour and a gradient, one step.
        root.store.beginUndoGroup(qsTr("Reset %1 to its group")
                                  .arg(root.labelForRole(key)))
        root.store.resetGradient(key)
        root.store.resetColor(key)
        root.store.endUndoGroup()
        root.editingStop = root.startingStop(key, -1)
        picker.show(root.pickerColourFor(key, root.editingStop))
        if (group.length > 0)
            root.announce(qsTr("%1 follows %2 again.")
                          .arg(root.labelForRole(key))
                          .arg(root.labelForRole(group)),
                          root.undoEdit)
    }
    // "Apply to all <group>": the group takes this element's colour (and
    // gradient) and every element of the group follows it again.
    function applyToGroup() {
        var key = root.editingRole
        if (key.length === 0 || root.editingGroup.length === 0)
            return
        var group = root.editingGroup
        root.store.sealUndoStep()
        var hex = root.toHex(Qt.color(String(root.effectiveColor(key))))
        if (root.store.applyToGroup(key, hex)) {
            root.editingStop = root.startingStop(key, -1)
            picker.show(root.pickerColourFor(key, root.editingStop))
            root.announce(qsTr("%1 now gives its colour to all of %2.")
                          .arg(root.labelForRole(key))
                          .arg(root.labelForRole(group)),
                          root.undoEdit)
        }
    }

    // The open role is a group and some of its elements keep a colour of their
    // own: how many.
    readonly property int editingKeepOwn: {
        var key = root.editingRole
        if (key.length === 0)
            return 0
        var members = root.store.groupMembers(key)
        var n = 0
        for (var i = 0; i < members.length; ++i) {
            if (root.isOverridden(members[i])
                    || (root.storeGradients
                        && root.storeGradients[members[i]] !== undefined))
                ++n
        }
        return n
    }

    // "Include them": the open group's colour goes to every element of it,
    // dropping what they had of their own.
    function includeOwnMembers() {
        var key = root.editingRole
        if (key.length === 0)
            return
        var members = root.store.groupMembers(key)
        if (members.length === 0)
            return
        var probe = ""
        for (var i = 0; i < members.length; ++i) {
            if (!root.hasOwnGradient(members[i])) {
                probe = members[i]
                break
            }
        }
        var hex = root.toHex(Qt.color(String(root.effectiveColor(key))))
        root.store.beginUndoGroup(qsTr("Include the elements of %1")
                                  .arg(root.labelForRole(key)))
        if (probe.length > 0) {
            // One step: the store applies the group's colour and frees every
            // member in a single write.
            root.store.applyToGroup(probe, hex)
        } else {
            for (var j = 0; j < members.length; ++j) {
                root.store.resetGradient(members[j])
                root.store.resetColor(members[j])
            }
        }
        root.store.endUndoGroup()
        root.announce(qsTr("Every element of %1 follows it now.")
                      .arg(root.labelForRole(key)),
                      root.undoEdit)
    }

    // "Edit <group>": the colour the open element is linked to.
    function editParent() {
        var parentKey = root.editingParent
        if (parentKey.length > 0)
            root.beginEdit(parentKey, root.labelForRole(parentKey))
    }

    // Unlinking keeps what is on screen: the colour the element has now
    // (and the gradient it was showing from its group) becomes its own, so
    // nothing changes until it is edited.
    readonly property bool editingCanUnlink:
        root.editingRole.length > 0 && root.editingParent.length > 0
        && Qt.color(String(root.effectiveColor(root.editingRole))).a > 0.999
    function unlinkEditing() {
        var key = root.editingRole
        var parentKey = root.editingParent
        if (key.length === 0 || parentKey.length === 0
                || !root.editingCanUnlink)
            return
        // The colour and the inherited gradient it copies are one step.
        root.store.beginUndoGroup(qsTr("Unlink %1").arg(root.labelForRole(key)))
        if (root.store.isGradientRole(key) && !root.hasOwnGradient(key)) {
            var inherited = AppTheme.inheritedGradient(
                                key, root.storeGradients, root.overrideColors)
            if (inherited)
                root.store.setGradient(key, inherited)
        }
        root.store.setColor(
            key, root.toHex(Qt.color(String(root.effectiveColor(key)))))
        root.store.endUndoGroup()
        root.announce(qsTr("%1 is no longer linked to %2.")
                      .arg(root.labelForRole(key))
                      .arg(root.labelForRole(parentKey)),
                      root.undoEdit)
    }

    // "Make readable": moves the open colour, and nothing else, to the nearest
    // lightness that clears every bar it fails against its neighbours. A
    // nudge, not a guarantee: the readability list still says what is left.
    function makeReadable(roleArg) {
        var key = roleArg !== undefined && roleArg !== "" ? roleArg
                                                          : root.editingRole
        // The open role is edited in the picker, which may be on a gradient
        // colour; any other role is just a flat colour.
        var open = key === root.editingRole
        if (key.length === 0 || (open && root.editingStop >= 0))
            return
        var pk = root.store.paletteKeyForRole(key)
        var rows = root.store.auditForRole(root.auditPalette, key)
        var current = root.toHex(Qt.color(String(root.effectiveColor(key))))
        var next = current
        var failing = 0
        var fixed = 0
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.passes)
                continue
            ++failing
            var other = r.fg === pk ? r.bg : (r.bg === pk ? r.fg : "")
            if (other.length === 0)
                continue
            var candidate = root.store.readableColor(
                                next, String(root.auditPalette[other]),
                                r.minimum, r.kind === "edge")
            if (candidate.length === 0)
                continue
            next = candidate
            ++fixed
        }
        if (failing === 0)
            return
        if (fixed === 0 || next === current) {
            root.announce(qsTr("No lightness of %1 clears the bar. Try changing the colour beside it.")
                          .arg(root.labelForRole(key)), null)
            return
        }
        root.store.sealUndoStep()
        root.store.nameNextStep(qsTr("Make %1 readable")
                                .arg(root.labelForRole(key)))
        root.store.setColor(key, next)
        root.store.sealUndoStep()
        if (open)
            picker.show(next)
        root.announce(qsTr("%1 is easier to read now.")
                      .arg(root.labelForRole(key)), root.undoEdit)
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
        root.reportOpen = true
    }

    function beginEdit(key, label) {
        // A new role is a new undo step, even if the last drag never ended.
        root.store.sealUndoStep()
        // The "under the pointer" chips stay while hopping between them.
        if (root.spotRoles.indexOf(key) < 0)
            root.spotRoles = []
        root.ownColoursAtOpen = root.overrideColors
        root.editingStop = root.startingStop(key, -1)
        root.editingRole = key
        root.editingLabel = label
        // The panel holds one thing at a time; opening a colour takes it.
        root.reportOpen = false
        picker.load(root.pickerColourFor(key, root.editingStop))
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

    // role -> label, built once: the sample window asks for every region's
    // name, and a list search per ask meant copying the roles a hundred times.
    readonly property var roleLabelMap: {
        var out = {}
        var list = root.store.roles
        for (var i = 0; i < list.length; ++i)
            out[list[i].key] = list[i].label
        return out
    }
    function labelForRole(key) {
        var v = root.roleLabelMap[key]
        return v !== undefined ? v : key
    }
    // One line a screen reader adds to an element: whether its colour is its
    // own or follows another.
    function describeRoleState(role) {
        if (root.isOverridden(role))
            return qsTr("Own colour")
        var p = root.liveLinks[role]
        return p !== undefined ? qsTr("Linked to %1").arg(root.labelForRole(p)) : ""
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

    // The four colours Simple edits; everything else follows them through the
    // groups.
    readonly property var simpleSwatches: {
        var keys = ["background", "surface", "textPrimary", "accent"]
        var out = []
        for (var i = 0; i < keys.length; ++i)
            out.push({ key: keys[i], label: root.labelForRole(keys[i]),
                       color: root.effectiveColor(keys[i]) })
        return out
    }
    // The surfaces that can be a gradient, offered at Simple so gradients are
    // found without reading the list.
    readonly property var simpleGradientRoles: {
        var keys = ["background", "sidebar", "rail", "surface"]
        var out = []
        for (var i = 0; i < keys.length; ++i) {
            if (root.store.isGradientRole(keys[i]))
                out.push({ key: keys[i], label: root.labelForRole(keys[i]),
                           color: root.effectiveColor(keys[i]) })
        }
        return out
    }
    readonly property string changedWord: qsTr("changed").toLowerCase()
    readonly property string hardWord: qsTr("unreadable").toLowerCase()

    // { role: true } for the roles the audit flags, to dot them in the preview
    // and to find them by searching "unreadable".
    readonly property var problemRoleSet: {
        var out = {}
        var rows = root.readabilityReport
        for (var i = 0; i < rows.length; ++i)
            out[rows[i].role] = true
        return out
    }

    // Groups narrowed by the filter; empty groups are dropped.
    //
    // Within a section every element role sits under the group it inherits
    // from (its `follows`), indented by `depth`, so "Panels and cards" lists
    // the room header, the side panel and the menus beneath it. A section is
    // the one its top-level group names; an element whose group lives in
    // another section is listed there, under that group. A filter hit on an
    // element keeps its group above it for context; a hit on a group keeps
    // its elements (typing "panels" shows everything that follows it).
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
        var byKey = {}
        var children = {}
        var i
        for (i = 0; i < list.length; ++i) {
            byKey[list[i].key] = list[i]
            var parentKey = list[i].follows
            if (parentKey && parentKey.length > 0) {
                if (children[parentKey] === undefined)
                    children[parentKey] = []
                children[parentKey].push(list[i].key)
            }
        }
        var shown = {}
        // Groups lists the colours others follow and Elements lists every
        // part; a search always looks at everything.
        var depthCap = root.level === 2 || needle.length > 0 ? 6 : 0
        var changedHit = needle.length >= 3
                         && root.changedWord.indexOf(needle) === 0
        var hardHit = needle.length >= 3
                      && root.hardWord.indexOf(needle) === 0
        var problemSet = hardHit ? root.problemRoleSet : ({})
        for (i = 0; i < list.length; ++i) {
            var textHit = needle.length > 0
                && (list[i].label.toLowerCase().indexOf(needle) >= 0
                    || list[i].group.toLowerCase().indexOf(needle) >= 0
                    || list[i].hint.toLowerCase().indexOf(needle) >= 0
                    || list[i].key.toLowerCase().indexOf(needle) >= 0)
            var wordHit = textHit && hexNeedle.length === 0
            var hit = needle.length === 0 || textHit
            // "grad…" lists the surfaces that can be a gradient.
            if (!hit && needle.length >= 3
                    && root.gradientWord.indexOf(needle) === 0
                    && root.store.isGradientRole(list[i].key))
                hit = true
            // "changed" lists what this theme sets itself, "unreadable" what
            // the audit flags.
            if (!hit && changedHit
                    && (root.isOverridden(list[i].key)
                        || (root.storeGradients
                            && root.storeGradients[list[i].key] !== undefined)))
                hit = true
            if (!hit && hardHit && problemSet[list[i].key] === true)
                hit = true
            if (!hit && hexNeedle.length > 0) {
                var hex = root.toHex(Qt.color(String(
                              root.effectiveColor(list[i].key))))
                hit = hex.toLowerCase().indexOf(hexNeedle) === 0
            }
            if (!hit)
                continue
            shown[list[i].key] = true
            // A word hit on a group's own name brings its elements; a colour
            // hit, or the "grad…" shortcut, is about this role alone.
            if (wordHit) {
                var stack = (children[list[i].key] || []).slice()
                for (var guard = 0; stack.length > 0 && guard < 400; ++guard) {
                    var c = stack.pop()
                    shown[c] = true
                    var grand = children[c]
                    if (grand)
                        stack = stack.concat(grand)
                }
            }
        }
        // Context: every shown element keeps the chain of groups above it.
        for (var key in shown) {
            var p = byKey[key] ? byKey[key].follows : ""
            for (var hops = 0; p && p.length > 0 && hops < 8; ++hops) {
                shown[p] = true
                p = byKey[p] ? byKey[p].follows : ""
            }
        }
        var emit = function (k, depth, section) {
            var r = byKey[k]
            out[seen[section]].items.push({
                key: r.key, label: r.label, hint: r.hint, group: r.group,
                follows: r.follows, depth: depth,
                members: children[k] ? children[k].length : 0
            })
            var kids = children[k] || []
            for (var j = 0; j < kids.length; ++j) {
                if (shown[kids[j]] && depth < depthCap)
                    emit(kids[j], depth + 1, section)
            }
        }
        for (i = 0; i < list.length; ++i) {
            var k = list[i].key
            var parent = list[i].follows
            // Elements are emitted under their group.
            if (!shown[k] || (parent && parent.length > 0 && byKey[parent]))
                continue
            var g = list[i].group
            if (seen[g] === undefined) {
                seen[g] = out.length
                out.push({ name: g, items: [] })
            }
            emit(k, 0, g)
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
            elide: Text.ElideRight
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
        enabled: root.opened
        onActivated: root.peelEscape()
    }
    // Detail level, and the search field.
    Shortcut {
        sequence: "Ctrl+1"
        enabled: root.opened
        onActivated: root.setLevel(0)
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: root.opened
        onActivated: root.setLevel(1)
    }
    Shortcut {
        sequence: "Ctrl+3"
        enabled: root.opened
        onActivated: root.setLevel(2)
    }
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: root.opened && !root.pickerPanelCovers
        onActivated: {
            filterField.forceActiveFocus()
            filterField.selectAll()
        }
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

                // The theme being edited, as the button that opens the theme
                // menu: switch, rename, new, duplicate, share, import, delete.
                Rectangle {
                    id: themeMenuButton
                    objectName: "themeMenuButton"
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: Math.min(240, menuNameLabel.implicitWidth
                                                 + menuChevron.width
                                                 + AppTheme.spacing12 * 2
                                                 + AppTheme.spacing8)
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                    color: root.menuOpen ? AppTheme.editorSelection
                         : menuButtonHover.containsMouse ? AppTheme.editorInset
                                                         : "transparent"
                    border.width: root.menuOpen ? 2 : 1
                    border.color: root.menuOpen ? AppTheme.editorAccent
                                                : AppTheme.editorBorderStrong
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Theme: %1. Open the theme menu")
                                     .arg(menuNameLabel.text)
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.menuOpen = !root.menuOpen
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: themeMenuButton.activeFocus
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.editorAccent
                    }
                    Label {
                        id: menuNameLabel
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.right: menuChevron.left
                        anchors.rightMargin: AppTheme.spacing6
                        anchors.verticalCenter: parent.verticalCenter
                        textFormat: Text.PlainText
                        text: root.store.name.length > 0 ? root.store.name
                                                         : qsTr("Untitled")
                        color: AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    Icon {
                        id: menuChevron
                        anchors.right: parent.right
                        anchors.rightMargin: AppTheme.spacing8
                        anchors.verticalCenter: parent.verticalCenter
                        name: root.menuOpen ? "expand_less" : "expand_more"
                        size: 16
                        color: AppTheme.editorTextSecondary
                    }
                    MouseArea {
                        id: menuButtonHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.menuOpen = !root.menuOpen
                    }
                }

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
                            id: readabilityBadge
                            objectName: "themeReadabilityBadge"
                            implicitWidth: verdictLabel.implicitWidth
                                           + AppTheme.spacing8 * 2
                            implicitHeight: 22
                            radius: AppTheme.radiusPill
                            // The one way into the report, so it is always
                            // there. The "could not check" clause waits for an
                            // edit: a stock base always has an ungradable
                            // pair, and it would qualify every pristine theme.
                            visible: true
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
                            Accessible.description:
                                qsTr("Open the readability report")
                            Keys.onPressed: (e) => {
                                if (e.key === Qt.Key_Return
                                    || e.key === Qt.Key_Enter
                                    || e.key === Qt.Key_Space) {
                                    root.reportOpen = !root.reportOpen
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
                                        && (root.store.overrideCount > 0
                                            || root.gradientCount > 0)
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
                                onClicked: root.reportOpen = !root.reportOpen
                            }
                        }
                    }
                }

                ThemeLevelSwitch {
                    Layout.alignment: Qt.AlignVCenter
                    level: root.level
                    onLevelRequested: (n) => root.setLevel(n)
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
                        Accessible.description: root.undoTip
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: root.undoTip
                        onClicked: root.undoEdit()
                    }
                    EditorButton {
                        objectName: "themeRedoButton"
                        visible: !root.confirmingReset
                        enabled: root.store.canRedo
                        text: qsTr("Redo")
                        Accessible.description: root.redoTip
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: root.redoTip
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

                    // Collapsed by default: the full grid takes about 235px
                    // permanently for a control used once per theme, starving
                    // the role list. One row: the label and the current base,
                    // which is itself the toggle (two rows cost a role's worth
                    // of height on a small window).
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.level > 0
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
                        visible: root.basesExpanded && root.level > 0
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
                                        root.requestBase(baseChip.modelData.id)
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
                                    onClicked: root.requestBase(baseChip.modelData.id)
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

                    ThemeSimplePanel {
                        id: simplePanel
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.level === 0
                                 && root.roleFilter.trim().length === 0
                        swatches: root.simpleSwatches
                        gradientRoles: root.simpleGradientRoles
                        baseTheme: root.store.baseTheme
                        bases: AppTheme.themeList.filter(
                                   (t) => t.id !== 12 && t.id !== 0)
                        problems: root.readabilityProblems
                        unchecked: root.readabilityTranslucent
                        dark: AppTheme.relativeLuminance(
                                  AppTheme._asColor(
                                      root.previewPalette.background)) < 0.18
                        openRole: root.editingRole
                        onRoleRequested: (key) =>
                            root.beginEdit(key, root.labelForRole(key))
                        onBaseRequested: (id) => root.requestBase(id)
                        onReportRequested: root.openReport()
                    }

                    Flickable {
                        id: roleScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.level > 0
                                 || root.roleFilter.trim().length > 0
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
                                            // An element sits under the group it
                                            // inherits from.
                                            Layout.leftMargin:
                                                Math.min(3, modelData.depth || 0)
                                                * AppTheme.spacing12
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
                                                        // What a group carries, below
                                                        // Elements, where its members
                                                        // are not listed.
                                                        Label {
                                                            objectName: "themeRoleMembers_"
                                                                + roleRow.modelData.key
                                                            visible: root.level < 2
                                                                     && roleRow.modelData.members > 0
                                                            textFormat: Text.PlainText
                                                            text: qsTr("%n element(s)",
                                                                       "custom theme, size of a group",
                                                                       roleRow.modelData.members)
                                                            color: AppTheme.editorTextMuted
                                                            font.family: AppTheme.uiFont
                                                            font.pixelSize: AppTheme.menuSectionSize
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
                            // Amber dots on what the audit flags.
                            problemRoles: root.problemRoleSet
                            // Names for the keyboard and screen readers, and
                            // Escape back to the list.
                            roleLabel: root.labelForRole
                            roleState: root.describeRoleState
                            onKeyboardEscape: {
                                if (root.reportOpen || root.shareOpen
                                        || root.menuOpen || root.pendingBase >= 0)
                                    root.dismissTransient()
                                else {
                                    // Narrow: the open colour's panel covers the
                                    // list and disables it, so the list can take
                                    // focus only once that panel is put away.
                                    if (root.pickerPanelCovers)
                                        root.editingRole = ""
                                    filterField.forceActiveFocus()
                                }
                            }
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

                // Asked over the preview, where the eye already is.
                ThemePresetPrompt {
                    id: presetPrompt
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: AppTheme.spacing24 * 2
                    z: 5
                    visible: root.pendingBase >= 0
                    presetName: root.presetName(root.pendingBase)
                    changes: root.store.overrideCount + root.gradientCount
                    onKeepRequested: root.resolvePendingBase(false)
                    onReplaceRequested: root.resolvePendingBase(true)
                    onCancelRequested: root.pendingBase = -1
                }

                // Said over the preview, where the eye already is.
                ThemeToast {
                    id: toast
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: AppTheme.spacing24 * 2
                    onActionTriggered: {
                        var action = root.toastUndoAction
                        if (action)
                            action()
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

    // The theme menu, under the theme button.
    ThemePopover {
        id: themeMenuPopover
        objectName: "themeMenuPopover"
        anchors.fill: parent
        anchorItem: themeMenuButton
        opened: root.menuOpen
        popoverWidth: 340
        accessibleTitle: qsTr("Themes")
        initialFocus: themeMenuPanel.nameField
        onDismissRequested: root.menuOpen = false

        ThemeMenuPanel {
            id: themeMenuPanel
            Layout.fillWidth: true
            themes: root.store.themes
            activeId: root.store.activeThemeId
            themeName: root.store.name
            canDuplicate: root.store.exists
            canCreate: root.store.themes.length < 24
            canDelete: root.store.exists
            confirmingDelete: root.confirmingDelete
            onThemeRequested: (id) => {
                root.store.activeThemeId = id
                root.editingRole = ""
            }
            onNameEdited: (text) => root.store.name = text
            onNewRequested: {
                root.store.createTheme("")
                root.editingRole = ""
            }
            onDuplicateRequested: root.store.duplicateActiveTheme("")
            onShareRequested: root.openShare("share")
            onImportRequested: root.openShare("import")
            // Asks first: a deleted theme cannot be undone, and its history
            // goes with it.
            onDeleteRequested: root.confirmingDelete = true
            onDeleteCancelled: root.confirmingDelete = false
            onDeleteConfirmed: {
                root.confirmingDelete = false
                root.store.deleteTheme(root.store.activeThemeId)
                root.editingRole = ""
            }
        }
    }

    // Share and Import, with a preview of what an import would do.
    ThemePopover {
        id: themeSharePopover
        objectName: "themeSharePopover"
        anchors.fill: parent
        anchorItem: themeMenuButton
        opened: root.shareOpen
        popoverWidth: 380
        accessibleTitle: qsTr("Share or import a theme")
        initialFocus: root.shareMode === "import" ? themeSharePanel.importField
                                                  : null
        onDismissRequested: root.shareOpen = false

        ThemeSharePanel {
            id: themeSharePanel
            Layout.fillWidth: true
            mode: root.shareMode
            themeName: root.store.name
            colourCount: root.store.overrideCount
            gradientCount: root.gradientCount
            problems: root.readabilityProblems
            copyNote: root.copyNote
            importText: root.importText
            preview: root.importPreview
            currentName: root.store.name
            onModeRequested: (mode) => root.shareMode = mode
            onCopyRequested: root.copyThemeCode()
            onImportTextEdited: (text) => root.importText = text
            onImportAsNewRequested: root.importAsNew()
            onReplaceRequested: root.replaceWithImport()
            onClearRequested: root.importText = ""
        }
    }

    // The readability report, under the header's readability pill.
    ThemePopover {
        id: readabilityPopover
        objectName: "themeReadabilityPopover"
        anchors.fill: parent
        anchorItem: readabilityBadge
        opened: root.reportOpen
        popoverWidth: 400
        accessibleTitle: qsTr("Readability report")
        onDismissRequested: root.reportOpen = false

        ThemeReadabilityPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            report: root.readabilityReport
            gradedPalette: root.auditPalette
            problems: root.readabilityProblems
            edited: root.store.overrideCount > 0 || root.gradientCount > 0
            translucent: root.readabilityTranslucent
            missing: root.readabilityMissing
            onFindingRequested: (role) =>
                root.beginEdit(role, root.labelForRole(role))
            onFixRequested: (role) => root.makeReadable(role)
            onCloseRequested: root.reportOpen = false
        }
    }

    // A theme file (or its code) dropped anywhere on the editor opens the
    // Import tab with its preview. It takes no mouse input of its own.
    DropArea {
        id: themeDrop
        objectName: "themeDropArea"
        anchors.fill: parent
        z: 90
        onDropped: (drop) => root.handleDrop(drop)

        Rectangle {
            objectName: "themeDropHint"
            anchors.fill: parent
            anchors.margins: AppTheme.spacing8
            visible: themeDrop.containsDrag
            radius: AppTheme.radiusMd
            color: Qt.alpha(AppTheme.editorAccent, 0.14)
            border.width: 3
            border.color: AppTheme.editorAccent
            Label {
                anchors.centerIn: parent
                text: qsTr("Drop a theme file to see what it would do")
                color: AppTheme.editorText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
        }
    }

    // The inspector panel, one instance with two homes. Wide: the reserved
    // third column, showing the picker when a role is open and a hint
    // otherwise. Compact: an overlay on the role column while editing.
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

                    // How the open role is tied to the rest of the theme, as
                    // things to act on: linked to a group (edit it, or unlink),
                    // or its own (reset it, or give it to the group).
                    ThemeLinkRow {
                        id: linkRow
                        Layout.fillWidth: true
                        visible: root.editingRole.length > 0
                                 && (root.editingParent.length > 0
                                     || root.editingGroup.length > 0
                                     || root.editingDependents.length > 0
                                     || root.editingKeepOwn > 0)
                        groupLabel: root.editingGroup.length > 0
                                    ? root.labelForRole(root.editingGroup) : ""
                        parentLabel: root.editingParent.length > 0
                                     ? root.labelForRole(root.editingParent) : ""
                        hasOwn: root.editingHasOwn
                                && root.editingFollowsDeclared.length > 0
                        canUnlink: root.editingCanUnlink
                        emphasizeParent: root.level < 2
                        alsoChanges: root.editingDependents.length
                        keepOwn: root.editingKeepOwn
                        showingDependents: root.showDependents
                        onParentRequested: root.editParent()
                        onUnlinkRequested: root.unlinkEditing()
                        onResetRequested: root.resetToGroup()
                        onApplyRequested: root.editingGroup.length > 0
                                          ? root.applyToGroup()
                                          : root.includeOwnMembers()
                        onShowDependentsToggled:
                            root.showDependents = !root.showDependents
                    }
                    Flow {
                        objectName: "themeRoleDependents"
                        Layout.fillWidth: true
                        visible: root.showDependents
                                 && root.editingDependents.length > 0
                        spacing: AppTheme.spacing4
                        Repeater {
                            model: root.showDependents ? root.editingDependents : []
                            delegate: RoleChip {
                                required property string modelData
                                roleKey: modelData
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

                        // How many of this colour's pairs fail right now.
                        readonly property int failing: {
                            var rows = roleChecks.model
                            var n = 0
                            for (var i = 0; rows && i < rows.length; ++i) {
                                if (rows[i].passes === false)
                                    ++n
                            }
                            return n
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing4
                            spacing: AppTheme.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Readability")
                                color: AppTheme.editorTextMuted
                                font.family: AppTheme.menuSectionFont
                                font.pixelSize: AppTheme.menuSectionSize
                                font.weight: AppTheme.menuSectionWeight
                            }
                            EditorButton {
                                objectName: "themeMakeReadable"
                                visible: roleReadability.failing > 0
                                         && root.editingStop < 0
                                implicitHeight: 26
                                text: qsTr("Make readable")
                                Accessible.description:
                                    qsTr("Move this colour to the nearest one that is easy to read against its neighbours")
                                onClicked: root.makeReadable()
                            }
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
                                                  checkRow.modelData.value.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                            : qsTr("ΔL* %1").arg(
                                                  checkRow.modelData.value.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
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
                                              checkRow.modelData.minimum.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                    color: AppTheme.editorTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                        }
                    }
                }
            }

            // Nothing open: say how to open something, and where the
            // readability verdict went.
            ColumnLayout {
                objectName: "themeInspectorEmpty"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.editingRole.length === 0
                spacing: AppTheme.spacing8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Nothing selected")
                    color: AppTheme.editorText
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Click a part of the sample window in the middle, or find a colour in the list on the left, and its colour opens here.")
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                EditorButton {
                    objectName: "themeReportOpenButton"
                    text: root.readabilityProblems > 0
                          ? qsTr("See what is hard to read")
                          : qsTr("Readability report")
                    onClicked: root.reportOpen = true
                }
                Item { Layout.fillHeight: true }
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
