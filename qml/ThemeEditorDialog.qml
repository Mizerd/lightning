import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

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

    property string initialRole: ""
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

    readonly property bool hasTransientState:
        root.confirmingReset || root.confirmingDelete
        || root.editingRole.length > 0
        || root.reportOpen || root.menuOpen || root.shareOpen
        || root.pendingBase >= 0 || root.selectingRoles

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
        if (root.reportOpen) {
            root.reportOpen = false
            readabilityBadge.forceActiveFocus()
        } else if (root.shareOpen)
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
        else if (root.selectingRoles)
            root.setSelecting(false)
    }

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
        if (root.editingRole.length > 0) {
            root.editingStop = root.startingStop(root.editingRole, root.editingStop)
            picker.show(root.pickerColourFor(root.editingRole, root.editingStop))
        } else {
            root.editingStop = -1
        }
    }

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

    readonly property bool narrowHeader: root.width < 1100
    readonly property int headerHeight: root.narrowHeader ? 112 : 68
    readonly property bool compact: root.width < 1380
    readonly property int workColumnWidth:
        Math.max(268, Math.min(330, Math.round(root.width * 0.235)))
    readonly property int pickerColumnWidth:
        Math.max(272, Math.min(304, Math.round(root.width * 0.222)))
    readonly property int pickerPanelX:
        root.compact ? 0 : root.width - root.pickerColumnWidth
    readonly property int pickerPanelWidth:
        root.compact ? root.workColumnWidth : root.pickerColumnWidth
    property bool reportOpen: false
    readonly property bool pickerPanelVisible:
        !root.compact || root.editingRole.length > 0
    readonly property bool pickerPanelCovers:
        root.compact && root.pickerPanelVisible

    property var auditPalette: root.previewPalette
    onPreviewPaletteChanged: if (!auditTimer.running) auditTimer.start()
    Timer {
        id: auditTimer
        interval: 120
        onTriggered: root.auditPalette = root.previewPalette
    }

    readonly property var readabilityReport:
        root.store.auditWithGradients(root.auditPalette, root.previewGradients)
    readonly property int readabilityProblems: root.readabilityReport.length

    readonly property var readabilitySkipped:
        root.store.auditSkipped(root.auditPalette, "")
    readonly property int readabilityUnchecked: root.readabilitySkipped.length
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
    property int editingStop: -1
    readonly property bool editingTakesGradient:
        root.editingRole.length > 0 && root.store.isGradientRole(root.editingRole)
    readonly property var storeGradients: root.store.gradients
    readonly property int gradientCount:
        root.storeGradients ? Object.keys(root.storeGradients).length : 0
    property string hoverRole: ""

    property bool selectingRoles: false
    property var selectedRoles: []
    property string groupError: ""
    property bool moveIntoGroup: false
    readonly property var colorGroups: root.store.colorGroups
    function groupFor(key) {
        var groups = root.colorGroups
        for (var i = 0; i < groups.length; ++i) {
            if (groups[i].members.indexOf(key) >= 0)
                return groups[i]
        }
        return null
    }
    function isSelected(key) { return root.selectedRoles.indexOf(key) >= 0 }
    function toggleSelected(key) {
        var next = root.selectedRoles.slice()
        var at = next.indexOf(key)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(key)
        root.selectedRoles = next
        root.groupError = ""
    }
    function setSelecting(on) {
        root.selectingRoles = on
        root.selectedRoles = []
        root.groupError = ""
        root.moveIntoGroup = false
    }
    readonly property var selectionImpact:
        root.selectedRoles.length > 0
        ? root.store.colorGroupImpact(root.selectedRoles)
        : ({ members: [], dependents: [] })
    readonly property int selectionInGroups: {
        var n = 0
        for (var i = 0; i < root.selectedRoles.length; ++i) {
            if (root.groupFor(root.selectedRoles[i]) !== null)
                ++n
        }
        return n
    }
    readonly property var selectionWithGradient:
        root.selectedRoles.filter((k) => root.hasOwnGradient(k))
    readonly property string selectionStartColour:
        root.selectedRoles.length > 0
        ? root.toHex(Qt.color(String(root.effectiveColor(root.selectedRoles[0]))))
        : ""
    function groupSelection(name) {
        if (root.selectedRoles.length < 2)
            return false
        var result = root.store.createColorGroup(
            name, root.selectedRoles, root.selectionStartColour,
            root.moveIntoGroup)
        if (!result.ok) {
            root.groupError = result.error
            return false
        }
        var count = root.selectedRoles.length
        root.setSelecting(false)
        root.announce(qsTr("Linked %n element(s)", "custom theme colour group",
                           count), function () { root.undoEdit() })
        return true
    }
    readonly property string activeIdForSelection: root.store.activeThemeId
    onActiveIdForSelectionChanged: root.setSelecting(false)
    readonly property var editingColorGroup:
        root.editingRole.length > 0 ? root.groupFor(root.editingRole) : null
    property bool confirmingReset: false
    property bool confirmingDelete: false
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
    property int pendingBase: -1
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
    function requestBase(id) {
        if (id === root.store.baseTheme)
            return
        if (root.store.overrideCount > 0 || root.gradientCount > 0)
            root.pendingBase = id
        else
            root.store.baseTheme = id
    }
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

    property bool basesExpanded: false
    property string roleFilter: ""
    property int level: 0
    property bool levelChosen: false
    function setLevel(n) {
        root.levelChosen = true
        root.level = Math.max(0, Math.min(2, n))
    }
    property bool showDependents: false
    property string shareMode: "share"
    property string importText: ""
    property string copyNote: ""

    function openShare(mode) {
        root.shareMode = mode
        root.menuOpen = false
        root.shareOpen = true
    }

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
            root.announce(text, importNoticeTimer.replaced ? root.undoEdit : null)
        }
    }

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

    TextEdit {
        id: themeClipboard
        visible: false
        width: 0
        height: 0
    }

    readonly property var previewPalette: AppTheme.paletteForTheme(12)

    readonly property var previewGradients: {
        var out = {}
        var own = root.store.gradients
        if (own) {
            for (var k in own)
                out[k] = own[k]
        }
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

    readonly property var overrideColors: root.store.colors
    readonly property var basePalette:
        AppTheme.paletteForTheme(root.store.baseTheme)

    function effectiveColor(rolekey) {
        var overrides = root.overrideColors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var alias = root.storeKeyAliases[rolekey]
        var lookup = alias !== undefined ? alias : rolekey
        var resolved = root.previewPalette
        if (resolved && resolved[lookup] !== undefined)
            return resolved[lookup]
        var pal = root.basePalette
        if (pal[lookup] !== undefined)
            return pal[lookup]
        return AppTheme.editorTextMuted
    }

    readonly property var rawBasePalette:
        AppTheme.rawPaletteForTheme(root.store.baseTheme)

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
    readonly property bool editingHasOwn:
        root.editingRole.length > 0
        && (root.isOverridden(root.editingRole)
            || (root.storeGradients
                && root.storeGradients[root.editingRole] !== undefined))

    property var toastUndoAction: null
    function announce(text, undoAction) {
        root.toastUndoAction = undoAction ? undoAction : null
        toast.actionHint = undoAction && root.store.undoLabel.length > 0
                           ? qsTr("Undo: %1").arg(root.store.undoLabel) : ""
        toast.show(text, undoAction ? qsTr("Undo") : "")
    }

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

    function resetToGroup() {
        var key = root.editingRole
        if (key.length === 0)
            return
        var group = root.editingGroup
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

    function editParent() {
        var parentKey = root.editingParent
        if (parentKey.length > 0)
            root.beginEdit(parentKey, root.labelForRole(parentKey))
    }

    readonly property bool editingCanUnlink:
        root.editingRole.length > 0 && root.editingParent.length > 0
        && Qt.color(String(root.effectiveColor(root.editingRole))).a > 0.999
    function unlinkEditing() {
        var key = root.editingRole
        var parentKey = root.editingParent
        if (key.length === 0 || parentKey.length === 0
                || !root.editingCanUnlink)
            return
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

    function drawnColoursOf(paletteKey) {
        var g = root.previewGradients
        if (g) {
            for (var k in g) {
                if (root.store.paletteKeyForRole(k) === paletteKey
                        && g[k] && g[k].stops && g[k].stops.length > 0) {
                    var out = []
                    for (var i = 0; i < g[k].stops.length; ++i)
                        out.push(String(g[k].stops[i]))
                    return out
                }
            }
        }
        return [String(root.auditPalette[paletteKey])]
    }

    function readableConstraints(key) {
        var scope = [key].concat(root.dependentsOf(key))
        var mine = {}
        for (var i = 0; i < scope.length; ++i)
            mine[root.store.paletteKeyForRole(scope[i])] = true
        var out = []
        for (var j = 0; j < scope.length; ++j) {
            var rows = root.store.auditForRole(root.auditPalette, scope[j])
            for (var n = 0; n < rows.length; ++n) {
                var r = rows[n]
                if (r.value === undefined || (mine[r.fg] === true) === (mine[r.bg] === true))
                    continue
                var others = root.drawnColoursOf(mine[r.fg] === true ? r.bg : r.fg)
                for (var c = 0; c < others.length; ++c)
                    out.push({ against: others[c], minimum: r.minimum,
                               edge: r.kind === "edge" })
            }
        }
        return out
    }
    function constraintFailures(hex, constraints) {
        var n = 0
        for (var i = 0; i < constraints.length; ++i) {
            var q = constraints[i]
            if (root.store.contrastRatio(hex, q.against) + 0.0005 < q.minimum)
                ++n
        }
        return n
    }

    function betterWithoutNewFailures(current, next, constraints) {
        var fewer = root.constraintFailures(next, constraints)
                    < root.constraintFailures(current, constraints)
        for (var i = 0; i < constraints.length && fewer; ++i) {
            var one = [constraints[i]]
            if (root.constraintFailures(current, one) === 0
                    && root.constraintFailures(next, one) > 0)
                return false
        }
        return fewer
    }

    function makeReadable(roleArg) {
        var key = roleArg !== undefined && roleArg !== "" ? roleArg
                                                          : root.editingRole
        var open = key === root.editingRole
        if (key.length === 0 || (open && root.editingStop >= 0))
            return
        var constraints = root.readableConstraints(key)
        var current = root.toHex(Qt.color(String(root.effectiveColor(key))))
        var before = root.constraintFailures(current, constraints)
        if (before === 0) {
            root.announce(qsTr("%1 already clears every bar it is checked against. If it is listed, the colour behind it is the one to change.")
                          .arg(root.labelForRole(key)), null)
            return
        }
        var next = current
        for (var pass = 0; pass < 3; ++pass) {
            var moved = false
            for (var i = 0; i < constraints.length; ++i) {
                var q = constraints[i]
                var candidate = root.store.readableColor(next, q.against,
                                                         q.minimum, q.edge)
                if (candidate.length > 0
                        && candidate.toUpperCase() !== String(next).toUpperCase()) {
                    next = candidate
                    moved = true
                }
            }
            if (!moved)
                break
        }
        if (next === current
                || !root.betterWithoutNewFailures(current, next, constraints)) {
            root.announce(qsTr("No lightness of %1 clears the bar without breaking another. Try changing the colour beside it.")
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

    readonly property string editingParent: {
        var p = root.liveLinks[root.editingRole]
        return p !== undefined ? p : ""
    }

    readonly property var storeKeyAliases: root.store.roleAliases()

    function isOverridden(rolekey) {
        var overrides = root.overrideColors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    function openReport() {
        root.reportOpen = true
    }

    function beginEdit(key, label) {
        root.store.sealUndoStep()
        if (root.spotRoles.indexOf(key) < 0)
            root.spotRoles = []
        root.ownColoursAtOpen = root.overrideColors
        root.editingStop = root.startingStop(key, -1)
        root.editingRole = key
        root.editingLabel = label
        root.reportOpen = false
        picker.load(root.pickerColourFor(key, root.editingStop))
    }

    function openFromPreview(role, stack) {
        if (root.selectingRoles) {
            root.toggleSelected(role)
            return
        }
        root.spotRoles = stack && stack.length > 1 ? stack : []
        root.beginEdit(role, root.labelForRole(role))
    }

    property var spotRoles: []
    property var ownColoursAtOpen: ({})

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

    readonly property var simpleSwatches: {
        var keys = ["background", "surface", "textPrimary", "accent"]
        var out = []
        for (var i = 0; i < keys.length; ++i)
            out.push({ key: keys[i], label: root.labelForRole(keys[i]),
                       color: root.effectiveColor(keys[i]) })
        return out
    }
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

    readonly property var problemRoleSet: {
        var out = {}
        var rows = root.readabilityReport
        for (var i = 0; i < rows.length; ++i)
            out[rows[i].role] = true
        return out
    }

    readonly property var roleGroups: {
        var out = []
        var seen = {}
        var needle = root.roleFilter.trim().toLowerCase()
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
        var depthCap = root.level === 2 || needle.length > 0
                       || root.selectingRoles ? 6 : 0
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
            if (!hit && needle.length >= 3
                    && root.gradientWord.indexOf(needle) === 0
                    && root.store.isGradientRole(list[i].key))
                hit = true
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
                border.color: btn.primary ? AppTheme.editorAccentInk
                                          : AppTheme.editorAccent
            }
        }
        Keys.onReturnPressed: (e) => { btn.click(); e.accepted = true }
        Keys.onEnterPressed: (e) => { btn.click(); e.accepted = true }
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
        sequences: [StandardKey.Redo, "Ctrl+Y"]
        enabled: root.opened && root.store.canRedo
        onActivated: root.redoEdit()
        onActivatedAmbiguously: root.redoEdit()
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.opened
        onActivated: root.peelEscape()
    }
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
        enabled: root.opened
        onActivated: {
            if (root.pickerPanelCovers)
                root.editingRole = ""
            filterField.forceActiveFocus()
            filterField.selectAll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.headerHeight
            color: AppTheme.editorPanel

            GridLayout {
                id: headerGrid
                objectName: "themeEditorHeader"
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                columnSpacing: AppTheme.spacing16
                rowSpacing: AppTheme.spacing4
                columns: root.narrowHeader ? 3 : 4

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
                            Accessible.name: text
                            HoverHandler { id: subtitleHover }
                            ToolTip.visible: subtitleHover.hovered && truncated
                            ToolTip.delay: 400
                            ToolTip.text: text
                        }

                        Rectangle {
                            id: readabilityBadge
                            objectName: "themeReadabilityBadge"
                            implicitWidth: verdictLabel.implicitWidth
                                           + AppTheme.spacing8 * 2
                            implicitHeight: 22
                            radius: AppTheme.radiusPill
                            visible: true
                            color: verdictHover.containsMouse
                                   ? AppTheme.editorSelection
                                   : AppTheme.editorInset
                            border.width: 1
                            border.color: root.readabilityProblems > 0
                                          ? AppTheme.editorDanger
                                          : AppTheme.editorBorderStrong
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

                Flickable {
                    id: headerActions
                    objectName: "themeHeaderActions"
                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                    Layout.columnSpan: root.narrowHeader ? 3 : 1
                    Layout.minimumWidth: Math.min(headerActionRow.implicitWidth, 200)
                    Layout.preferredWidth: headerActionRow.implicitWidth
                    implicitHeight: headerActionRow.implicitHeight
                    contentWidth: headerActionRow.implicitWidth
                    contentHeight: headerActionRow.implicitHeight
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    Connections {
                        target: headerActions.Window.window
                        function onActiveFocusItemChanged() {
                            let it = headerActions.Window.window
                                     ? headerActions.Window.window.activeFocusItem
                                     : null
                            let p = it
                            while (p && p !== headerActionRow)
                                p = p.parent
                            if (!p)
                                return
                            const x = it.mapToItem(headerActionRow, 0, 0).x
                            if (x < headerActions.contentX)
                                headerActions.contentX = Math.max(0, x)
                            else if (x + it.width > headerActions.contentX
                                                    + headerActions.width)
                                headerActions.contentX = Math.min(
                                    Math.max(0, headerActions.contentWidth
                                                - headerActions.width),
                                    x + it.width - headerActions.width)
                        }
                    }

                    Row {
                        id: headerActionRow
                        spacing: AppTheme.spacing8

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

            Rectangle {
                id: workColumn
                Layout.preferredWidth: root.workColumnWidth
                Layout.minimumWidth: root.workColumnWidth
                Layout.maximumWidth: root.workColumnWidth
                Layout.fillHeight: true
                color: AppTheme.editorPanel

                enabled: !root.pickerPanelCovers

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

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        EditorButton {
                            objectName: "themeSelectRolesButton"
                            Layout.fillWidth: true
                            text: root.selectingRoles ? qsTr("Done selecting")
                                                      : qsTr("Select elements to link")
                            checkable: true
                            checked: root.selectingRoles
                            Accessible.role: Accessible.CheckBox
                            Accessible.checked: root.selectingRoles
                            Accessible.description:
                                qsTr("Choose two or more elements and give them one shared colour")
                            onClicked: root.setSelecting(!root.selectingRoles)
                        }
                    }
                    Rectangle {
                        objectName: "themeGroupSelectionBar"
                        visible: root.selectingRoles
                        Layout.fillWidth: true
                        implicitHeight: selectionCol.implicitHeight + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: 1
                        border.color: AppTheme.editorBorderStrong
                        ColumnLayout {
                            id: selectionCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing6
                            Label {
                                objectName: "themeGroupSelectionCount"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: root.selectedRoles.length === 0
                                      ? qsTr("Click elements in the list or the preview to select them.")
                                      : qsTr("%n element(s) selected", "custom theme colour group",
                                             root.selectedRoles.length)
                                color: AppTheme.editorText
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                            }
                            Label {
                                objectName: "themeGroupSelectionImpact"
                                Layout.fillWidth: true
                                visible: root.selectionImpact.dependents.length > 0
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: qsTr("Also changes %n element(s) that follow them: %1",
                                           "custom theme colour group",
                                           root.selectionImpact.dependents.length)
                                      .arg(root.selectionImpact.dependents
                                           .map((d) => d.label).join(", "))
                                color: AppTheme.editorTextSecondary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.menuSectionSize
                            }
                            Label {
                                objectName: "themeGroupSelectionGradient"
                                Layout.fillWidth: true
                                visible: root.selectionWithGradient.length > 0
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: qsTr("%1 has a gradient. A group links solid colours: remove the gradient first or leave it out.")
                                      .arg(root.selectionWithGradient
                                           .map((k) => root.labelForRole(k)).join(", "))
                                color: AppTheme.editorDanger
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.menuSectionSize
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.selectedRoles.length >= 2
                                spacing: AppTheme.spacing6
                                Rectangle {
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    radius: AppTheme.radiusSm
                                    color: root.selectionStartColour.length > 0
                                           ? root.selectionStartColour : "transparent"
                                    border.width: 1
                                    border.color: AppTheme.editorBorderStrong
                                }
                                Label {
                                    objectName: "themeGroupStartColour"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    textFormat: Text.PlainText
                                    text: root.selectedRoles.length > 0
                                          ? qsTr("Starts with %1 (the colour of %2)")
                                            .arg(root.selectionStartColour)
                                            .arg(root.labelForRole(root.selectedRoles[0]))
                                          : ""
                                    color: AppTheme.editorTextSecondary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.menuSectionSize
                                }
                            }
                            CheckBox {
                                id: moveCheck
                                objectName: "themeGroupMoveCheck"
                                visible: root.selectionInGroups > 0
                                checked: root.moveIntoGroup
                                onToggled: root.moveIntoGroup = checked
                                text: qsTr("Move %n element(s) out of their current group",
                                           "custom theme colour group",
                                           root.selectionInGroups)
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.menuSectionSize
                            }
                            Rectangle {
                                visible: root.selectedRoles.length >= 2
                                Layout.fillWidth: true
                                implicitHeight: 30
                                radius: AppTheme.radiusMd
                                color: AppTheme.editorPanel
                                border.width: groupNameField.activeFocus ? 2 : 1
                                border.color: groupNameField.activeFocus
                                              ? AppTheme.editorAccent
                                              : AppTheme.editorBorderStrong
                                TextInput {
                                    id: groupNameField
                                    objectName: "themeGroupNameField"
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
                                    Accessible.name: qsTr("Group name")
                                    onAccepted: if (root.groupSelection(text)) text = ""
                                    Label {
                                        anchors.fill: parent
                                        verticalAlignment: Text.AlignVCenter
                                        visible: groupNameField.text.length === 0
                                        text: qsTr("Group name")
                                        color: AppTheme.editorTextMuted
                                        font: groupNameField.font
                                    }
                                }
                            }
                            EditorButton {
                                objectName: "themeGroupCreateButton"
                                visible: root.selectedRoles.length >= 2
                                Layout.fillWidth: true
                                primary: true
                                enabled: root.selectionWithGradient.length === 0
                                         && (root.selectionInGroups === 0
                                             || root.moveIntoGroup)
                                text: qsTr("Link colours")
                                onClicked: if (root.groupSelection(groupNameField.text))
                                               groupNameField.text = ""
                            }
                            Label {
                                objectName: "themeGroupError"
                                Layout.fillWidth: true
                                visible: root.groupError.length > 0
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: root.groupError
                                color: AppTheme.editorDanger
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.menuSectionSize
                            }
                        }
                    }

                    ThemeSimplePanel {
                        id: simplePanel
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.level === 0
                                 && root.roleFilter.trim().length === 0
                                 && !root.selectingRoles
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
                                 || root.selectingRoles
                        clip: true
                        contentWidth: width
                        contentHeight: roleColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

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

                            ColumnLayout {
                                objectName: "themeColorGroups"
                                visible: root.colorGroups.length > 0
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.topMargin: AppTheme.spacing6
                                    text: qsTr("Colour groups")
                                    textFormat: Text.PlainText
                                    color: AppTheme.editorTextSecondary
                                    font.family: AppTheme.menuSectionFont
                                    font.pixelSize: AppTheme.menuSectionSize
                                    font.weight: AppTheme.menuSectionWeight
                                }
                                Repeater {
                                    model: root.colorGroups
                                    delegate: Rectangle {
                                        id: groupRow
                                        required property var modelData
                                        objectName: "themeColorGroup_" + modelData.id
                                        property bool renaming: false
                                        Layout.fillWidth: true
                                        implicitHeight: Math.max(38, groupRowLayout.implicitHeight + 8)
                                        radius: AppTheme.radiusSm
                                        color: groupHover.containsMouse ? AppTheme.editorInset
                                                                        : "transparent"
                                        activeFocusOnTab: true
                                        Accessible.role: Accessible.Button
                                        Accessible.name: qsTr("Colour group %1").arg(modelData.name)
                                        Accessible.description: modelData.labels.join(", ")
                                        Keys.onPressed: (e) => {
                                            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                                || e.key === Qt.Key_Space) {
                                                root.beginEdit(groupRow.modelData.members[0],
                                                    root.labelForRole(groupRow.modelData.members[0]))
                                                e.accepted = true
                                            }
                                        }
                                        Rectangle {
                                            anchors.fill: parent
                                            visible: groupRow.activeFocus
                                            radius: AppTheme.radiusSm
                                            color: "transparent"
                                            border.width: 2
                                            border.color: AppTheme.editorAccent
                                        }
                                        MouseArea {
                                            id: groupHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.beginEdit(groupRow.modelData.members[0],
                                                root.labelForRole(groupRow.modelData.members[0]))
                                        }
                                        ColumnLayout {
                                            id: groupRowLayout
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: AppTheme.spacing8
                                            anchors.rightMargin: AppTheme.spacing6
                                            spacing: AppTheme.spacing4
                                          RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            Rectangle {
                                                implicitWidth: 26
                                                implicitHeight: 26
                                                radius: AppTheme.radiusSm
                                                color: groupRow.modelData.color
                                                border.width: 1
                                                border.color: AppTheme.editorBorderStrong
                                                Icon {
                                                    anchors.centerIn: parent
                                                    name: "link"
                                                    size: 14
                                                    color: AppTheme.relativeLuminance(
                                                               Qt.color(groupRow.modelData.color)) > 0.4
                                                           ? "#000000" : "#FFFFFF"
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Label {
                                                    visible: !groupRow.renaming
                                                    Layout.fillWidth: true
                                                    textFormat: Text.PlainText
                                                    text: groupRow.modelData.name
                                                    color: AppTheme.editorText
                                                    font.family: AppTheme.uiFont
                                                    font.pixelSize: AppTheme.textMeta
                                                    elide: Label.ElideRight
                                                }
                                                TextInput {
                                                    id: renameInput
                                                    objectName: "themeColorGroupRename_" + groupRow.modelData.id
                                                    visible: groupRow.renaming
                                                    Layout.fillWidth: true
                                                    color: AppTheme.editorText
                                                    font.family: AppTheme.uiFont
                                                    font.pixelSize: AppTheme.textMeta
                                                    maximumLength: 48
                                                    clip: true
                                                    Accessible.role: Accessible.EditableText
                                                    Accessible.name: qsTr("Group name")
                                                    onAccepted: {
                                                        root.store.renameColorGroup(
                                                            groupRow.modelData.id, text)
                                                        groupRow.renaming = false
                                                    }
                                                    Keys.onEscapePressed: groupRow.renaming = false
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    textFormat: Text.PlainText
                                                    text: groupRow.modelData.labels.join(", ")
                                                    color: AppTheme.editorTextMuted
                                                    font.family: AppTheme.uiFont
                                                    font.pixelSize: AppTheme.menuSectionSize
                                                    elide: Label.ElideRight
                                                }
                                            }
                                          }
                                          RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 26 + AppTheme.spacing8
                                            spacing: AppTheme.spacing6
                                            EditorButton {
                                                objectName: "themeColorGroupRenameButton_" + groupRow.modelData.id
                                                implicitHeight: 26
                                                implicitWidth: implicitContentWidth + AppTheme.spacing8 * 2
                                                text: qsTr("Rename")
                                                Accessible.name: qsTr("Rename %1").arg(groupRow.modelData.name)
                                                onClicked: {
                                                    renameInput.text = groupRow.modelData.name
                                                    groupRow.renaming = true
                                                    renameInput.forceActiveFocus()
                                                }
                                            }
                                            EditorButton {
                                                objectName: "themeColorGroupUngroup_" + groupRow.modelData.id
                                                implicitHeight: 26
                                                implicitWidth: implicitContentWidth + AppTheme.spacing8 * 2
                                                text: qsTr("Ungroup")
                                                Accessible.name: qsTr("Ungroup %1, keeping its colours")
                                                                 .arg(groupRow.modelData.name)
                                                onClicked: {
                                                    var name = groupRow.modelData.name
                                                    if (root.store.deleteColorGroup(groupRow.modelData.id))
                                                        root.announce(qsTr("Ungrouped %1; the colours stay").arg(name),
                                                                      function () { root.undoEdit() })
                                                }
                                            }
                                          }
                                        }
                                    }
                                }
                            }

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
                                            readonly property bool seeThrough:
                                                resolved.a < 0.999
                                            readonly property var gradientSpec: {
                                                var g = root.storeGradients
                                                return g && g[modelData.key]
                                                       ? g[modelData.key] : null
                                            }
                                            readonly property string followsKey: {
                                                var p = root.liveLinks[modelData.key]
                                                return p !== undefined ? p : ""
                                            }
                                            Layout.fillWidth: true
                                            Layout.leftMargin:
                                                Math.min(3, modelData.depth || 0)
                                                * AppTheme.spacing12
                                            implicitHeight: 38
                                            radius: AppTheme.radiusSm
                                            color: editing ? AppTheme.editorSelection
                                                 : roleHover.containsMouse
                                                   ? AppTheme.editorInset
                                                   : "transparent"

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
                                                    roleRow.activate()
                                                    e.accepted = true
                                                }
                                            }
                                            function activate() {
                                                if (root.selectingRoles)
                                                    root.toggleSelected(roleRow.modelData.key)
                                                else
                                                    root.beginEdit(roleRow.modelData.key,
                                                                   roleRow.modelData.label)
                                            }
                                            readonly property var colorGroup:
                                                root.groupFor(modelData.key)
                                            Accessible.checkable: root.selectingRoles
                                            Accessible.checked: root.selectingRoles
                                                                && root.isSelected(modelData.key)
                                            Rectangle {
                                                anchors.fill: parent
                                                visible: roleRow.activeFocus
                                                radius: AppTheme.radiusSm
                                                color: "transparent"
                                                border.width: 2
                                                border.color: AppTheme.editorAccent
                                            }
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
                                            onEditingChanged:
                                                if (editing) revealInList()

                                            MouseArea {
                                                id: roleHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: roleRow.activate()
                                            }

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: AppTheme.spacing8
                                                anchors.rightMargin: AppTheme.spacing6
                                                spacing: AppTheme.spacing8

                                                Rectangle {
                                                    objectName: "themeRoleSelect_"
                                                        + roleRow.modelData.key
                                                    visible: root.selectingRoles
                                                    implicitWidth: 16
                                                    implicitHeight: 16
                                                    radius: 3
                                                    color: root.isSelected(roleRow.modelData.key)
                                                           ? AppTheme.editorAccent : "transparent"
                                                    border.width: 1
                                                    border.color: root.isSelected(roleRow.modelData.key)
                                                                  ? AppTheme.editorAccent
                                                                  : AppTheme.editorBorderStrong
                                                    Icon {
                                                        anchors.centerIn: parent
                                                        visible: root.isSelected(roleRow.modelData.key)
                                                        name: "check"
                                                        size: 12
                                                        color: AppTheme.editorAccentInk
                                                    }
                                                }

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
                                                        textFormat: Text.PlainText
                                                        Layout.fillWidth: true
                                                        text: roleRow.modelData.label
                                                        color: AppTheme.editorText
                                                        font.family: AppTheme.uiFont
                                                        font.pixelSize: AppTheme.textMeta
                                                        elide: Label.ElideRight
                                                    }
                                                    RowLayout {
                                                        Layout.fillWidth: true
                                                        spacing: AppTheme.spacing6
                                                        Label {
                                                            objectName: "themeRoleHex_"
                                                                + roleRow.modelData.key
                                                            textFormat: Text.PlainText
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
                                                            elide: Label.ElideRight
                                                            Layout.maximumWidth: implicitWidth
                                                            Layout.minimumWidth: 0
                                                        }
                                                        Label {
                                                            objectName: "themeRoleGroup_"
                                                                + roleRow.modelData.key
                                                            visible: roleRow.colorGroup !== null
                                                            Layout.minimumWidth: 0
                                                            textFormat: Text.PlainText
                                                            text: roleRow.colorGroup !== null
                                                                  ? qsTr("linked: %1").arg(roleRow.colorGroup.name)
                                                                  : ""
                                                            color: AppTheme.editorAccent
                                                            font.family: AppTheme.uiFont
                                                            font.pixelSize: AppTheme.menuSectionSize
                                                            elide: Label.ElideRight
                                                        }
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
                                                            elide: Label.ElideRight
                                                            Layout.minimumWidth: 0
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
                            channels: app.settings
                                      && app.settings.roomNavigationLayout === 1
                            highlightRole: root.editingRole
                            hoverRole: root.hoverRole
                            gradients: root.previewGradients
                            linkedRoles: root.linkedRoleSet
                            problemRoles: root.problemRoleSet
                            roleLabel: root.labelForRole
                            roleState: root.describeRoleState
                            onKeyboardEscape: {
                                if (root.reportOpen || root.shareOpen
                                        || root.menuOpen || root.pendingBase >= 0)
                                    root.dismissTransient()
                                else {
                                    if (root.pickerPanelCovers)
                                        root.editingRole = ""
                                    filterField.forceActiveFocus()
                                }
                            }
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

            Item {
                Layout.preferredWidth: root.pickerColumnWidth
                Layout.minimumWidth: root.pickerColumnWidth
                Layout.maximumWidth: root.pickerColumnWidth
                Layout.fillHeight: true
                visible: !root.compact
            }
        }
    }

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
            onDeleteRequested: root.confirmingDelete = true
            onDeleteCancelled: root.confirmingDelete = false
            onDeleteConfirmed: {
                root.confirmingDelete = false
                root.store.deleteTheme(root.store.activeThemeId)
                root.editingRole = ""
            }
        }
    }

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
            id: readabilityPanel
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
            onCloseRequested: {
                root.reportOpen = false
                readabilityBadge.forceActiveFocus()
            }
        }
    }

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

    Rectangle {
        id: pickerPanel
        x: root.pickerPanelX
        y: root.headerHeight
        width: root.pickerPanelWidth
        height: Math.max(0, root.height - root.headerHeight)
        visible: root.pickerPanelVisible
        color: AppTheme.editorPanel

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

                    Rectangle {
                        objectName: "themeEditingGroupScope"
                        visible: root.editingColorGroup !== null
                                 && root.editingRole.length > 0
                        Layout.fillWidth: true
                        implicitHeight: groupScopeCol.implicitHeight + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: AppTheme.editorInset
                        border.width: 1
                        border.color: AppTheme.editorAccent
                        ColumnLayout {
                            id: groupScopeCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing6
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing6
                                Icon {
                                    Layout.alignment: Qt.AlignTop
                                    name: "link"
                                    size: 16
                                    color: AppTheme.editorAccent
                                }
                                Label {
                                    objectName: "themeEditingGroupScopeText"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    textFormat: Text.PlainText
                                    text: root.editingColorGroup !== null
                                          ? qsTr("Linked in %1: this colour changes all %n element(s): %2",
                                                 "custom theme colour group",
                                                 root.editingColorGroup.members.length)
                                            .arg(root.editingColorGroup.name)
                                            .arg(root.editingColorGroup.labels.join(", "))
                                          : ""
                                    color: AppTheme.editorText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.menuSectionSize
                                }
                            }
                            EditorButton {
                                objectName: "themeDetachFromGroupButton"
                                implicitHeight: 28
                                text: qsTr("Edit this element only")
                                onClicked: {
                                    var label = root.labelForRole(root.editingRole)
                                    if (root.store.detachFromColorGroup(root.editingRole))
                                        root.announce(qsTr("%1 now has its own colour").arg(label),
                                                      function () { root.undoEdit() })
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
                        canReset: root.editingRole.length > 0
                                  && root.editingStop < 0
                                  && root.isOverridden(root.editingRole)
                        resetLabel: root.editingFollowsDeclared.length > 0
                                    ? qsTr("Reset: follow %1 again")
                                      .arg(root.labelForRole(
                                          root.editingFollowsDeclared))
                                    : qsTr("Reset to the base theme")
                        suggestions: root.paletteSwatches
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
                            autoSelect: root.editingTakesGradient
                            onStopSelected: (index) => {
                                if (index >= 0 && !root.editingTakesGradient)
                                    return
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
                        onGestureFinished: root.store.sealUndoStep()
                        onResetRequested: {
                            if (root.editingRole.length > 0) {
                                root.store.resetColor(root.editingRole)
                                picker.show(root.effectiveColor(root.editingRole))
                                root.announce(qsTr("%1 was reset.")
                                              .arg(root.labelForRole(root.editingRole)),
                                              root.undoEdit)
                            }
                        }
                        onClosed: root.editingRole = ""
                    }

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

                    ColumnLayout {
                        id: roleReadability
                        objectName: "themeRoleReadability"
                        Layout.fillWidth: true
                        visible: root.editingRole.length > 0 && roleChecks.count > 0
                        spacing: 2

                        FontMetrics {
                            id: checkMetrics
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        readonly property int checkRowHeight:
                            2 * Math.ceil(checkMetrics.lineSpacing)

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
                            model: root.editingRole.length > 0
                                   ? root.store.auditForRole(root.auditPalette,
                                                             root.editingRole)
                                     .concat(root.store.auditSkipped(root.auditPalette,
                                                                     root.editingRole))
                                   : []
                            delegate: RowLayout {
                                id: checkRow
                                required property var modelData
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
                                    Layout.preferredHeight:
                                        roleReadability.checkRowHeight
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: roleReadability.checkRowHeight / 2
                                    textFormat: Text.PlainText
                                    text: checkRow.modelData.label
                                    color: AppTheme.editorTextSecondary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Label.ElideRight
                                }
                                Label {
                                    objectName: "themeRoleCheckValue"
                                    textFormat: Text.PlainText
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

    readonly property var paletteSwatches: {
        var pal = root.basePalette
        var keys = ["background", "sidebar", "rail", "surface", "cardElevated",
                    "hover", "selected", "border", "borderStrong",
                    "inputBackground", "accent", "link", "textPrimary",
                    "textSecondary", "textMuted", "ownBubble", "otherBubble"]
        var out = []
        var seen = {}
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
