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

    // ── Geometry ─────────────────────────────────────────────────────────
    //
    // THE SIDE COLUMNS USED TO BE PINNED AT 330 AND 320, AND THAT MADE THE
    // PREVIEW UNUSABLE ON ANY ORDINARY WINDOW. Measured on the GUI at four
    // sizes (the mock's natural size is 880x560, scaled down to fit and never
    // up):
    //
    //   1920 wide  scale 1.00   the size the mock was designed against
    //   1440x900   scale 0.852  preview 750x477, 45% of the column bare
    //   1024x680   scale 0.380  preview 334x213, 69% bare, 2 of 26 roles shown
    //    953x833   scale 0.299  preview 263x167, 81% bare — and 953 is the
    //                           window width recorded in the maintainer's own
    //                           config, so this is the ordinary case
    //    820x620   scale 0.148  preview 130x82
    //    640x420   NO PREVIEW AT ALL, and the Done button off-screen: the two
    //              fixed columns are 650px of a window the app's own
    //              `minimumWidth` (Main.qml) allows to be 640.
    //
    // Body text inside the mock is ~4px tall at 0.299. Judging a colour on
    // that is not possible, so on the maintainer's window the preview — the
    // entire justification for an inline picker rather than a modal
    // ColorDialog — was already hidden, by the editor's own furniture.
    //
    // Two changes. The columns are now RANGES rather than constants, and
    // below `compact` the picker stops being a third column and overlays the
    // ROLE column instead. That honours the recorded reason for the inline
    // picker exactly: the rule is that the picker must not cover the PREVIEW,
    // and the role list is the one thing a person does not need in the
    // instant after they clicked a role.
    //
    // The preview never re-flows when a role is selected, which is the OTHER
    // recorded promise: in wide mode the third column is reserved whether or
    // not a role is open (and now holds the readability report when it is
    // not), and in compact mode the picker is an overlay, so the stage width
    // is identical in both states either way.
    //
    // WHERE THE BREAKPOINT IS, AND WHY IT IS NOT LOWER. Three columns cost
    // ~0.457 of the width, two cost ~0.235, so below some width the third
    // column is buying a role list at the price of the preview. 1380 is where
    // three columns still deliver ~0.81 scale; under it they do not, so they
    // fold. Crossing the breakpoint therefore makes the preview JUMP larger,
    // which is deliberate and legible — the picker column visibly folds away
    // in the same frame, so the growth reads as the layout changing rather
    // than as a glitch.
    //
    // PREDICTED scale after this change, from the arithmetic below; the
    // measured values are in the round's report:
    //   1920  1.000 (capped)      1440  0.870 (was 0.852)
    //   1380  0.809               1379  1.000 (two columns)
    //   1024  0.814 (was 0.380)    953  0.733 (was 0.299)
    //    820  0.582 (was 0.148)    640  0.377 (was: no preview at all)
    readonly property int headerHeight: 68
    readonly property bool compact: root.width < 1380
    readonly property int workColumnWidth:
        Math.max(268, Math.min(330, Math.round(root.width * 0.235)))
    // Capped a little tighter than the role column: ColorPickerPanel's
    // natural width is 288 and everything in it stretches, so 304 costs it
    // one swatch per row and buys the preview 20px at every wide size.
    readonly property int pickerColumnWidth:
        Math.max(272, Math.min(304, Math.round(root.width * 0.222)))
    // What the picker occupies while it is open. In compact mode it is drawn
    // over the role column, so it costs the preview nothing.
    readonly property int pickerPanelX:
        root.compact ? 0 : root.width - root.pickerColumnWidth
    readonly property int pickerPanelWidth:
        root.compact ? root.workColumnWidth : root.pickerColumnWidth
    // IN COMPACT MODE THE PANEL HAS TO BE ASKED FOR, AND THE REPORT IS A
    // REASON TO ASK. The first cut showed it only while a role was being
    // edited, which made the readability report — this round's headline
    // surface — unreachable on any window under 1380, INCLUDING the
    // maintainer's own 953. `reportOpen` is what the header badge sets, so
    // the badge does the same thing in both modes: it puts the report in
    // front of you.
    property bool reportOpen: false
    readonly property bool pickerPanelVisible:
        !root.compact || root.editingRole.length > 0 || root.reportOpen
    // True while the panel is COVERING the role column rather than sitting
    // beside the preview. Everything underneath must stop taking input.
    readonly property bool pickerPanelCovers:
        root.compact && root.pickerPanelVisible

    // ── Readability ──────────────────────────────────────────────────────
    //
    // The findings for the palette as the application would actually paint
    // it. `previewPalette` is the SAME resolved object the preview renders,
    // so the report is about the pixels on screen rather than about the
    // sparse override map — which matters, because the commonest failures are
    // an interaction between a colour the user changed and one they inherited
    // from the base and never looked at.
    //
    // See CustomThemeStore's header for what is checked and why it is
    // calibrated against the eleven shipped presets.
    //
    // THROTTLED, AND THE MEASUREMENT IS WHY. Graded straight off
    // `previewPalette` the report re-evaluated once per MOUSE SAMPLE while the
    // picker was dragged: the whole ~40-key palette crossed into C++ twice
    // (once for the summary, once for the open role's live readout — FOUR
    // times since the skipped-check pass was added beside each) and the
    // report's Repeater tore down and rebuilt a delegate per finding — with
    // thirteen findings on screen that is ~780 delegate rebuilds over one
    // drag. Measured on the GUI, 60-sample drag, identical protocol and the
    // same binary:
    //
    //   the editor before this round      1050 / 1170 / 1180 ms CPU
    //   with the QML hoist, audit removed    0 /   10 /    0 ms
    //   with the QML hoist and an untimed audit
    //                                     1280 / 1320 / 1330 ms
    //
    // So the hoist took the per-sample cost to essentially nothing and an
    // unthrottled report handed all of it back and more. A trailing 120 ms
    // throttle cuts ~60 evaluations per drag to about five while a number
    // watched at 8 Hz still visibly climbs, which is all the live readout
    // needs to teach with.
    //
    // THROTTLE, NOT DEBOUNCE: `auditTimer` is only started when it is not
    // already running, so it fires DURING the drag rather than only after it
    // — and because every change restarts nothing, the last change always
    // leaves a pending fire, so the FINAL sample always lands. A debounce
    // would freeze the numbers for the whole gesture, and a throttle that
    // dropped its trailing edge would leave the report describing a colour
    // the user no longer has.
    property var auditPalette: root.previewPalette
    onPreviewPaletteChanged: if (!auditTimer.running) auditTimer.start()
    Timer {
        id: auditTimer
        interval: 120
        onTriggered: root.auditPalette = root.previewPalette
    }

    readonly property var readabilityReport: root.store.audit(root.auditPalette)
    readonly property int readabilityProblems: root.readabilityReport.length

    // WHAT COULD NOT BE CHECKED, WHICH IS NOT THE SAME AS WHAT PASSED.
    //
    // The store refuses to grade a pair whose colour is see-through, and it
    // is right to (a translucent fill composites over whatever is behind it,
    // so its contrast is unknowable). Nothing said so: measured on the Storm
    // base, the live readout under `textPrimary` listed SEVEN rows where the
    // table holds EIGHT checks naming it, and the header badge said
    // "Readable" over the pair that had never been graded. An unqualified
    // clean bill over a question nobody answered is the badge lying, however
    // honest the C++ under it.
    readonly property var readabilitySkipped:
        root.store.auditSkipped(root.auditPalette, "")
    readonly property int readabilityUnchecked: root.readabilitySkipped.length
    // The two reasons read completely differently to a user: "your colour is
    // see-through" is something they chose and can change, "this build has no
    // such colour" is a bug in us. A palette can hold BOTH, and that is the
    // realistic shape of the second one arriving: Storm always contributes a
    // translucent `hover`, so a renamed key would land beside it and a single
    // count under a single sentence would blame us for the user's colour or
    // the user for ours. Counted apart, and said apart.
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

    // The role currently open in the picker. Held on the dialog, not on the
    // row: a Repeater delegate can be destroyed while the picker is open (the
    // list scrolls, the group filter changes) and the pending role would go
    // with it.
    property string editingRole: ""
    property string editingLabel: ""
    property bool confirmingReset: false
    // The base-theme grid. Collapsed by default — see the comment beside it.
    property bool basesExpanded: false
    // What the role list is narrowed to. 26 roles across 6 groups is exactly
    // the size at which typing beats scrolling.
    property string roleFilter: ""
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
        // A SHARED THEME IS SOMEBODY ELSE'S WORK AND NOBODY CHECKED IT.
        // Both of the unreadable themes this round measured arrived exactly
        // this way — pasted, accepted, and confirmed with a cheerful line.
        // The count is deferred by a frame because the store has only just
        // emitted its change and `readabilityProblems` reads the palette
        // AppTheme resolves from it.
        //
        // AND THE AUDIT IS FORCED FORWARD FIRST, or the cheerful line counts
        // the WRONG THEME. `auditPalette` starts as a binding to
        // `previewPalette`, but the throttle below assigns to it imperatively
        // — and the first such assignment destroys the binding for good, so
        // from then on it only moves when that 120 ms timer fires. The notice
        // timer runs at 0 ms, so after one colour drag anywhere in the
        // session, an import reported the problem count of the theme you had
        // BEFORE importing. (The same shape this repo already records for
        // `Image.source`: an imperative write is not an update, it is the end
        // of the binding.)
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

    // THE TWO HOT INPUTS, READ ONCE PER CHANGE INSTEAD OF ONCE PER ROW.
    //
    // `effectiveColor` and `isOverridden` are called from the role list, which
    // instantiates every one of its 26 rows (there is no virtualisation in a
    // ColumnLayout). Each row asked for `store.colors` FOUR times — the
    // swatch's colour, its border width, its border colour, and the reset
    // button's `visible` — and `store.colors` is a Q_PROPERTY returning a
    // QVariantMap BY VALUE, so every one of those was a fresh
    // QVariantMap -> QJSValue conversion: 104 per repaint. `effectiveColor`
    // additionally called AppTheme.paletteForTheme() for every NON-overridden
    // role, and that function builds a fresh ~45-key object literal on every
    // call and caches nothing — up to 26 more per repaint.
    //
    // All of it re-ran on every `customThemeChanged`, which fires once per
    // MOUSE SAMPLE while the picker is dragged. Measured on the GUI before
    // this change: a 60-sample drag cost 680 ms of process CPU against a
    // 50 ms control (the same drag over dead canvas) and rendered 15 frames,
    // ~24 fps. Hoisting both onto `root` makes them one conversion and one
    // palette build per change, whatever the row count.
    readonly property var overrideColors: root.store.colors
    readonly property var basePalette:
        AppTheme.paletteForTheme(root.store.baseTheme)

    function effectiveColor(rolekey) {
        var overrides = root.overrideColors
        if (overrides && overrides[rolekey] !== undefined)
            return overrides[rolekey]
        var pal = root.basePalette
        // paletteForTheme resolves SEMANTIC role names; a few store keys are
        // the palette's own spelling (inputBg -> inputBackground, mention ->
        // mentionBadge, reaction -> reactionBackground).
        var alias = root.storeKeyAliases[rolekey]
        var lookup = alias !== undefined ? alias : rolekey
        if (pal[lookup] !== undefined)
            return pal[lookup]
        return AppTheme.editorTextMuted
    }

    // ONE MAP, AND IT IS THE STORE'S. This was an object literal here, and
    // the readability table in CustomThemeStore.cpp carried the same three
    // pairs a second time in its two key columns — two hand-kept copies of
    // one fact, neither asserted against the other. Read once into a property
    // because `effectiveColor` runs for all 26 rows on every repaint and this
    // dialog's hot inputs are hoisted for exactly that reason;
    // `everyCheckGradesTheColourItsRoleWouldEdit` is what keeps the two
    // spellings of every check in agreement now.
    readonly property var storeKeyAliases: root.store.roleAliases()

    function isOverridden(rolekey) {
        var overrides = root.overrideColors
        return overrides !== undefined && overrides[rolekey] !== undefined
    }

    // One verb, because the badge now has two ways in (pointer and keyboard)
    // and they must not drift apart.
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

    // CustomThemeStore stores #RRGGBB and nothing else; the picker hands back
    // a QML color, whose toString() is #AARRGGBB.
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

    // Groups, narrowed by the filter. A group with nothing left in it is
    // dropped rather than shown empty — a header over nothing is worse than
    // no header.
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

    // A plain Item, not the ColumnLayout directly: the picker/report panel
    // below is POSITIONED rather than laid out (it changes column in compact
    // mode), and a Rectangle parented to a ColumnLayout becomes a layout item
    // — it would be stacked under the header instead of floating over the
    // role column.
    contentItem: Item {

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.headerHeight
            color: AppTheme.editorPanel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                spacing: AppTheme.spacing16

                // ELIDING, AND THAT IS NOT COSMETIC. A Label with no elide
                // reports its full text as its implicit width, a RowLayout
                // will not shrink a child below that, and the button cluster
                // beside it is pushed off the window: measured at 640x420 —
                // a size the app's own `minimumWidth` allows — the header ran
                // to x=639 and DONE WAS COMPLETELY OFF-SCREEN, leaving Escape
                // as the only way out of the editor.
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

                        // THE VERDICT, ALWAYS ON SCREEN.
                        //
                        // The full report shares a panel with the picker, so
                        // it is not visible while a colour is open — the one
                        // thing that must never be hidden is the ANSWER. This
                        // is the whole state of the theme in one word, and
                        // clicking it opens the report: in wide mode by
                        // closing the picker, in compact mode by also raising
                        // the panel, which is otherwise down.
                        Rectangle {
                            objectName: "themeReadabilityBadge"
                            implicitWidth: verdictLabel.implicitWidth
                                           + AppTheme.spacing8 * 2
                            implicitHeight: 22
                            radius: AppTheme.radiusPill
                            // Shown whenever there is something to say, not
                            // only once the user has edited something: the
                            // report and the badge must never disagree about
                            // whether this theme has a problem.
                            //
                            // INCLUDING "we could not check one of them" —
                            // but ONLY where the badge is the sole route to
                            // that fact. In COMPACT mode the report is not on
                            // screen, so hiding the qualification there is
                            // how an unanswered question turns back into a
                            // pass. In WIDE mode the report column is
                            // permanent and already carries the sentence —
                            // and a new theme on the stock Storm base has one
                            // ungradable pair from the moment it is created,
                            // so an unconditional clause would qualify the
                            // header of every pristine theme before the user
                            // had touched anything. That is exactly the
                            // failure this table's own calibration is tuned
                            // to avoid: a warning that fires on a stock theme
                            // teaches people to ignore every warning. The
                            // TEXT change is what fixes the reported defect;
                            // this clause only decides where it can be read.
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
                            // Keyboard, for the same reason as the role rows
                            // and the base chips: this was a MouseArea with
                            // an Accessible.role and no way to reach it, and
                            // it is the control that opens this round's
                            // headline surface.
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
                                // A PASS THAT LEAVES SOMETHING UNANSWERED
                                // SAYS SO. "Readable" over a palette holding
                                // an ungradable pair is a claim this editor
                                // has not earned — see `readabilitySkipped`.
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
                id: workColumn
                Layout.preferredWidth: root.workColumnWidth
                Layout.minimumWidth: root.workColumnWidth
                Layout.maximumWidth: root.workColumnWidth
                Layout.fillHeight: true
                color: AppTheme.editorPanel

                // NOTHING UNDER THE OVERLAY MAY TAKE INPUT. In compact mode
                // the picker panel occupies exactly this column's rectangle,
                // and a plain Rectangle accepts no mouse buttons — so before
                // this line a click on the panel's own margins, its section
                // gaps or the filler below the picker fell straight through
                // to whatever role row was hidden underneath and silently
                // switched the role being edited. `enabled: false` also takes
                // the whole subtree out of the tab chain, so Tab cannot walk
                // into rows nobody can see.
                enabled: !root.pickerPanelCovers

                // The seam the picker column has always had, on the side that
                // never had one: the panel simply stopped and the canvas
                // began, so the editor was bordered on the right and not on
                // the left. A 1px asymmetry is small and it reads as
                // unfinished, which is the complaint this round is answering.
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

                    // COLLAPSED BY DEFAULT, AND THAT IS A SPACE DECISION.
                    //
                    // Eleven chips in a 2-up Flow is six rows — about 235px,
                    // held permanently, for a control touched ONCE per theme.
                    // The role list underneath it, which is the editor's whole
                    // job, got whatever was left: measured, 8 of 26 roles
                    // visible at 1440x900 and 2 of 26 at 1024x680. The
                    // allocation was the exact inverse of the use.
                    //
                    // Collapsed it still answers the question it exists to
                    // answer — which base am I on — because the current one is
                    // shown as its own chip, painted in its own palette.
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

                    // The current base, shown while the grid is collapsed.
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

                    // Base-theme chips, each painting its own palette. A
                    // combo box shows a NAME; this shows the thing the name
                    // refers to, which is the only useful question here.
                    Flow {
                        Layout.fillWidth: true
                        visible: root.basesExpanded
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

                                // Keyboard. A base chip was a Rectangle with a
                                // MouseArea, so it could not be reached at
                                // all: measured, the whole tab ring was eight
                                // stops — the header buttons, the name field
                                // and the five collection buttons — and not
                                // one of them changed a colour.
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
                                // Inset, not outset: an outset ring is drawn
                                // outside the chip's own bounds, which is the
                                // shape §16 records as making every
                                // neighbour's spacing budget wrong. This is
                                // the same inset EditorButton already uses.
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

                    // Narrowing the list. Twenty-six roles in six groups is
                    // where typing starts to beat scrolling, and it matters
                    // most on the small windows where only a handful of rows
                    // fit at all.
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
                            // ESCAPE MUST STILL CLOSE THE EDITOR.
                            // QQuickKeysAttached accepts the event BEFORE it
                            // calls a named-key handler, so an unconditional
                            // handler here swallows Escape and the dialog's
                            // own CloseOnEscape never fires — with the filter
                            // focused, the editor could not be closed from
                            // the keyboard at all. Clear the filter when there
                            // is one; otherwise hand the key on.
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

                                    // The group header was editorTextMuted at
                                    // menuSectionSize and measured close to
                                    // invisible in a capture — which left a
                                    // 26-row list with no navigation at all.
                                    // editorTextSecondary is the same
                                    // typographic rung with ink you can find.
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
                                            // THREE ANSWERS FOR ONE COLOUR,
                                            // RECONCILED. The swatch beside
                                            // this row paints a translucent
                                            // inherited value WITH its alpha,
                                            // the hex prints the opaque
                                            // triple, and the audit refuses
                                            // to grade it at all. Only the
                                            // third of those was ever
                                            // explained. Storm's `hover` is
                                            // Qt.alpha(_stoHover, 0.22), so
                                            // this is the stock state of a
                                            // fresh theme, not an edge case.
                                            readonly property bool seeThrough:
                                                resolved.a < 0.999
                                            Layout.fillWidth: true
                                            implicitHeight: 38
                                            radius: AppTheme.radiusSm
                                            color: editing ? AppTheme.editorSelection
                                                 : roleHover.containsMouse
                                                   ? AppTheme.editorInset
                                                   : "transparent"

                                            // Keyboard, for the same reason as
                                            // the base chips: the 26 rows that
                                            // ARE this editor were unreachable
                                            // without a pointer.
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
                                            // A Flickable does not follow
                                            // focus, so tabbing through 26
                                            // rows walked the focus ring
                                            // straight out of the clipped
                                            // viewport with nothing on screen
                                            // moving — the keyboard path
                                            // existed and was still unusable.
                                            //
                                            // mapToItem, not a bare `y`: the
                                            // row's direct parent is the
                                            // per-group ColumnLayout, so `y`
                                            // alone is group-relative.
                                            // `roleColumn` has no y of its
                                            // own, so its space and the
                                            // Flickable's content space
                                            // coincide.
                                            //
                                            // Clamped although the two
                                            // branches already bound
                                            // themselves — assigning
                                            // `contentY` does NOT clamp
                                            // (StopAtBounds governs dragging,
                                            // not assignment), so the clamp is
                                            // free insurance against a future
                                            // contentHeight that stops
                                            // tracking implicitHeight.
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

                                                // THE SWATCH IS THE DATA; THE
                                                // CHANGED MARK IS AN ANNOTATION
                                                // ON IT, AND IT USED TO BE THE
                                                // LOUDER OF THE TWO. A 2px
                                                // accent ring around a 24px
                                                // chip is a quarter of its
                                                // area, so on a theme with a
                                                // dozen overrides the list read
                                                // as a wall of blue rectangles
                                                // with the colours hidden
                                                // inside them. The mark is a
                                                // separate dot now and the
                                                // swatch keeps a neutral
                                                // outline whatever its state.
                                                // THE BADGE IS INSIDE ITS OWN
                                                // BOX. §16: a thing drawn
                                                // outside its own bounds makes
                                                // every neighbour's budget
                                                // wrong, and the obvious
                                                // spelling here — a 26px
                                                // swatch with the dot hung off
                                                // its corner at negative y —
                                                // is exactly that. The cell is
                                                // 30px, the swatch sits 4px
                                                // below its top edge, and the
                                                // dot occupies the corner the
                                                // swatch gave up. Nothing
                                                // overhangs, so the row's
                                                // spacing means what it says.
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
                                                        // Remote or externally chosen text: never markup.
                                                        textFormat: Text.PlainText
                                                        Layout.fillWidth: true
                                                        text: roleRow.modelData.label
                                                        color: AppTheme.editorText
                                                        font.family: AppTheme.uiFont
                                                        font.pixelSize: AppTheme.textMeta
                                                        elide: Label.ElideRight
                                                    }
                                                    // THE VALUE. Reading your
                                                    // own theme used to take 26
                                                    // clicks — the picker was
                                                    // the only place any hex
                                                    // was ever shown, one role
                                                    // at a time. Copying a tone
                                                    // from one role to another
                                                    // is the commonest thing a
                                                    // person does here.
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

            // ── Picker slot ──────────────────────────────────────────────
            // RESERVED, not occupied. The picker itself is positioned
            // manually below, because in compact mode it is drawn OVER the
            // role column rather than beside the preview, and one panel that
            // moves is the only way to keep a single instance of the picker's
            // HSV state. This item exists solely so the layout keeps the
            // preview off the reserved strip in wide mode — which is what
            // stops the preview re-flowing when a role is opened.
            Item {
                Layout.preferredWidth: root.pickerColumnWidth
                Layout.minimumWidth: root.pickerColumnWidth
                Layout.maximumWidth: root.pickerColumnWidth
                Layout.fillHeight: true
                visible: !root.compact
            }
        }
    }

    // ── The picker / report panel ────────────────────────────────────────
    //
    // One instance, two homes. Wide: the reserved third column, always
    // visible — the picker when a role is open, the readability report when
    // one is not, so the column is never the 320x830 rectangle holding two
    // sentences that it used to be (measured: 98.5% flat panel colour).
    // Compact: an overlay on the role column, shown only while editing.
    Rectangle {
        id: pickerPanel
        x: root.pickerPanelX
        y: root.headerHeight
        width: root.pickerPanelWidth
        height: Math.max(0, root.height - root.headerHeight)
        visible: root.pickerPanelVisible
        color: AppTheme.editorPanel

        // THE EVENT SINK, AND IT MUST BE DECLARED FIRST.
        //
        // A Rectangle's `acceptedMouseButtons` is Qt::NoButton, so the panel
        // consumed nothing: presses, hover, wheel and the cursor shape all
        // reached the column it is drawn over. Declared FIRST it is the
        // BOTTOM sibling, so every real control in the panel is above it and
        // still wins; it only catches what nothing else wanted. The explicit
        // arrow cursor is part of the fix — without it the hidden row's
        // PointingHandCursor showed through over bare panel.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
            onWheel: (wheel) => wheel.accepted = true
        }

        // THE SEAM IS POSITIONED, NOT ANCHORED, and the difference is a
        // whole column of the wrong colour.
        //
        // It was `anchors.left: compact ? undefined : parent.left` with the
        // mirror on `anchors.right`. The panel is built while `root.width` is
        // still 0, so `compact` is true and `right` binds; when the real
        // width arrives and `left` binds too, the anchor system has BOTH
        // edges, writes `width` directly, and destroys the `width: 1`
        // binding — which the later clearing of `right` can no longer undo.
        // A 1px rule became a 268px slab of `editorBorder` across the whole
        // picker/report column: MEASURED #4C596D on a dark base where
        // `editorPanel` is #2A3140, and #C4D2E7 on a light one.
        //
        // It put the readability panel's own text on the wrong surface, and
        // on a light base that text measured 4.44:1 — the panel that enforces
        // 4.5:1 failing its own bar. A source read cannot see any of this.
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

            // THE LIVE READOUT, AND THE REASON IT SHOWS PASSES TOO.
            //
            // Every pair the open role takes part in, graded as you drag.
            // A warning that merely disappears teaches nothing; a number
            // climbing past its bar while the crosshair moves is what makes
            // the relationship between a colour and its readability visible
            // at the moment the user can act on it.
            ColumnLayout {
                objectName: "themeRoleReadability"
                Layout.fillWidth: true
                visible: root.editingRole.length > 0 && roleChecks.count > 0
                spacing: 2

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
                    // The throttled palette, for the reason recorded beside
                    // `auditPalette`: this Repeater rebuilds a delegate per
                    // check, and bound to the live palette it did so once per
                    // mouse sample.
                    //
                    // THE UNGRADABLE PAIRS ARE IN THE SAME LIST, because
                    // leaving them out is what made this readout claim eight
                    // checks and show seven. A row that says "not checked" is
                    // an answer; a row that is absent is indistinguishable
                    // from a check that does not exist.
                    model: root.editingRole.length > 0
                           ? root.store.auditForRole(root.auditPalette,
                                                     root.editingRole)
                             .concat(root.store.auditSkipped(root.auditPalette,
                                                             root.editingRole))
                           : []
                    delegate: RowLayout {
                        id: checkRow
                        required property var modelData
                        // `passes` is ABSENT on a skipped row, never false —
                        // the store refuses to hand out a verdict it did not
                        // reach.
                        readonly property bool graded:
                            modelData.passes !== undefined
                        Layout.fillWidth: true
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
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            // The check's own written sentence. Composing it
                            // from the two role names gave "Text on accent on
                            // Accent" — see the phrase field in
                            // CustomThemeStore.cpp.
                            text: checkRow.modelData.label
                            color: AppTheme.editorTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            elide: Label.ElideRight
                        }
                        Label {
                            objectName: "themeRoleCheckValue"
                            textFormat: Text.PlainText
                            // A WCAG ratio reads as "4.6:1"; a lightness
                            // separation is not a ratio and must not be
                            // dressed as one.
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
                        // THE BAR THE NUMBER IS CLIMBING TOWARDS. Without it
                        // this row told the user they had failed and not by
                        // how much — and the whole argument for showing
                        // passes here is that a number moving against a
                        // TARGET is what teaches. The report's rows have said
                        // "4.3:1 — needs 4.5:1" all along; this is the same
                        // fact in the width a picker column has.
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

            // ── The report, when nothing is being edited ──────────────
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
                    // Only in compact, where the panel is an overlay the user
                    // raised on purpose and has to be able to put down again.
                    // In wide mode the column is permanent and a close button
                    // on it would be a control that does nothing.
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

                // WHAT WAS NOT CHECKED, SAID OUT LOUD. The verdict above is
                // about the pairs that could be graded, and until this line
                // existed nothing distinguished "every pair passed" from
                // "every pair we were able to look at passed".
                Label {
                    objectName: "themeReadabilityUnchecked"
                    visible: root.readabilityTranslucent > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    // WORDED TO SURVIVE n == 1. The English catalog has no
                    // numerus forms filled in for this family (the sibling
                    // "%n thing(s) hard to read" is still `unfinished`), so
                    // the SOURCE string is what a reader sees at every n —
                    // and "1 colour(s) … they are see-through" is worse than
                    // the plural-agnostic sentence below.
                    text: qsTr("%n colour(s) could not be checked: a see-through colour reads differently depending on what is behind it.",
                               "custom theme readability",
                               root.readabilityTranslucent)
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }

                // A SEPARATE SENTENCE BECAUSE IT IS A SEPARATE ACCUSATION.
                // Nothing the user did can produce this one — it means the
                // readability table names a palette key this build no longer
                // returns — and `everyReadabilityKeyIsAKeyPaletteForThemeReturns`
                // is what should make it unreachable. If a reader ever sees
                // it, the bug is ours and the wording says so.
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
                    // NAMED so a focused row can scroll itself into view.
                    // The 26 role rows have done this all along; these rows
                    // gained the keyboard and not the clamp, and this list
                    // routinely overflows — the suite's own Ink fixture
                    // produces TWENTY findings. Tabbing past the visible
                    // ones moved focus, and the focus ring with it, off
                    // screen: the reader this whole surface exists for.
                    id: problemScroll
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

                        Repeater {
                            model: root.readabilityReport
                            delegate: Rectangle {
                                id: problemRow
                                required property var modelData
                                objectName: "themeReadabilityRow"
                                Layout.fillWidth: true
                                // FIXED, and the label elides rather than
                                // wraps. A wrapping Label whose implicitHeight
                                // feeds its own row's height is the shape a
                                // Qt layout loop comes in, and a binding loop
                                // is a LOAD-TIME fact no source scan can see
                                // (§16). Uniform rows also read as a list
                                // rather than as a ragged stack.
                                implicitHeight: 44
                                radius: AppTheme.radiusSm
                                color: problemHover.containsMouse
                                       ? AppTheme.editorInset : "transparent"

                                // Keyboard, exactly as the 26 role rows have
                                // it. These rows ARE the report — the surface
                                // this round exists for — and they were
                                // pointer-only, which makes a readability
                                // feature unreachable to the readers most
                                // likely to need it.
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: problemText.text
                                // The role rows' clamp, mirrored: assigning
                                // `contentY` does NOT clamp (StopAtBounds
                                // governs dragging, not assignment), and
                                // `problemColumn` has no y of its own, so its
                                // space and the Flickable's content space
                                // coincide.
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

                                    // The two colours that are failing, one on
                                    // the other, at the size they fail at.
                                    // Naming a pair is abstract; showing it is
                                    // the argument.
                                    Rectangle {
                                        implicitWidth: 30
                                        implicitHeight: 30
                                        radius: AppTheme.radiusSm
                                        // The THROTTLED palette, not the live
                                        // one: these two colours are what the
                                        // row's ratio was computed from, and a
                                        // sample that had moved on from the
                                        // number beside it would be showing a
                                        // pair that does not have that ratio.
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
                                            Layout.fillWidth: true
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

    } // contentItem

    // The base theme's own colours, offered in the picker. Building a theme
    // almost always means reusing a tone that is already in it — a hand-typed
    // near-miss is how a palette loses its coherence.
    //
    // SORTED BY LIGHTNESS, not by semantic key order. The list used to be
    // emitted in the order written below, which on any dark theme front-loads
    // every near-black: a review counted NINE of the fifteen swatches as
    // indistinguishable navies at 26px. The stated purpose of the strip is
    // "reuse a tone that is already in here", and a strip whose entries the
    // eye cannot separate cannot serve it — the user types a near-miss
    // anyway. Sorting on CIE L* makes it read as a ladder, which is the same
    // lesson §16 records for the rail's tint ladder: equal steps in the data
    // are not equal steps to the eye.
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
