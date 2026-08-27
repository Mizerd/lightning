import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Choose what to share, on the platforms that have no portal.
//
// WHY THIS EXISTS ONLY OFF LINUX. The xdg desktop portal owns the picker
// there: it shows its own dialog, the user chooses in it, and Lightning
// receives a PipeWire node for exactly what was chosen — which is what makes
// sharing safe on Wayland, and why ScreenCastPortal never enumerates anything
// itself. Windows and macOS have no such broker, so the capture element takes
// a display index or a window handle and nothing was asking the user which
// one: a share silently took whichever display the app happened to be on.
//
// `app.groupCall.screenShareSources` is EMPTY on Linux, always, so this never
// opens there — two dialogs for one gesture would be worse than none. There
// is deliberately NO Linux branch anywhere below: an empty list already
// produces no dialog.
//
// A GRID, NOT A LIST (2026-08-27), because the list could not answer the
// question it was asked. Reported: "with brave it listed my tab name but
// didnt even say brave anywhere and from small preview hard to tell what that
// was". Two separate faults in one sentence — a 64px preview is not enough to
// recognise a window by sight, and a Chromium caption is the TAB's title and
// names no browser at all. So the preview is now the tile, the two kinds get
// a tab each instead of a header inside one list, and the resolution is gone
// from the face of the dialog entirely (it survives in Accessible.name, where
// it costs no space and still answers "which monitor is the 4K one?").
AppDialog {
    id: root

    title: qsTr("Choose what to share")
    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    // A ceiling, not a width: three 16:9 tiles need room, and the min() is
    // what keeps the dialog inside a small window. Nothing inside is sized in
    // absolute terms — every label elides or wraps — so a different font or a
    // 125% device pixel ratio changes the text, never the frame.
    width: Math.min(920, parent ? parent.width - 64 : 920)

    /// The rows to offer. Bound to the live call, and NOT readonly — the
    /// binding is the production path, and a test that cannot put rows in it
    /// cannot press one. That gap is not theoretical: the grouped-list rework
    /// shipped with its rows unclickable, and every check that existed passed
    /// because none of them had a row to click.
    property var sources: app.groupCall ? app.groupCall.screenShareSources : []

    /// THE INDEX INTO `sources`, and it must stay that.
    ///
    /// `SfuCallController::chooseScreenShareSource(index)` indexes into the
    /// UNFILTERED list it built, so a tab that shows a subset must map its
    /// delegate index back to the original one. Sharing the wrong thing is
    /// the worst outcome this dialog has, so the mapping is carried in the
    /// model itself (`sourceIndex` on every entry, see rowsForTab) rather
    /// than recomputed by arithmetic at the point of use.
    property int selected: 0

    // Split for the two tabs. A row is a WINDOW when it carries a non-zero
    // handle — the same fact the controller keys the capture off, so the
    // picker and the pipeline cannot disagree about what a row means.
    function isWindowRow(row) {
        return row !== undefined && row !== null
            && row.windowHandle !== undefined && row.windowHandle !== 0;
    }
    readonly property int screenCount: {
        var n = 0;
        for (var i = 0; i < sources.length; ++i)
            if (!isWindowRow(sources[i]))
                ++n;
        return n;
    }
    readonly property int windowCount: sources.length - screenCount

    // ---- tabs -------------------------------------------------------------
    //
    // Both tabs are ALWAYS offered, including an empty one, which then says so
    // in its own words. A tab that appears and disappears with the machine's
    // state is worse than an empty one: the user cannot learn where a thing
    // lives if the place moves.
    readonly property string tabApplications: "applications"
    readonly property string tabScreens: "screens"
    property string tab: tabScreens

    /// The rows of one tab, each carrying the index it has in `sources`.
    function rowsForTab(which) {
        var out = [];
        for (var i = 0; i < sources.length; ++i) {
            if (isWindowRow(sources[i]) === (which === root.tabApplications))
                out.push({ source: sources[i], sourceIndex: i });
        }
        return out;
    }
    readonly property var shownRows: root.rowsForTab(root.tab)
    /// Whether this build can list windows AT ALL — a different question from
    /// whether any are open, and the one the empty state needs in order not to
    /// claim a fact about the user's desktop that Lightning cannot know.
    readonly property bool windowCaptureSupported:
        app.groupCall ? app.groupCall.windowCaptureSupported : false

    /// Where `sources[idx]` sits in the visible tab, or -1 if it is elsewhere.
    ///
    /// COMPUTED, never read from `shownRows`, and the difference is a real
    /// defect rather than a style preference. `shownRows` is a BINDING on
    /// `root.tab`, and a change handler for `tab` can run BEFORE the bindings
    /// that depend on it have caught up. Measured: switching to Applications
    /// with a display selected ran `selectionIntoView()` against the still-
    /// stale SCREENS rows, so the rescue moved the highlight to the first
    /// screen — the dialog then showed two window tiles with nothing
    /// highlighted, and Share sent a display the user could not see chosen.
    /// Every imperative reader below therefore asks `rowsForTab` directly;
    /// only the GridView's model, which is the thing being updated, reads the
    /// binding.
    function viewIndexOf(idx) {
        var rows = root.rowsForTab(root.tab);
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].sourceIndex === idx)
                return i;
        return -1;
    }

    // ---- labelling --------------------------------------------------------
    //
    // TWO LINES, application first, and the caption on its own line below.
    //
    // The single-line alternative ("Brave Browser — Anthropic Console") also
    // leads with the application and also survives elision, but it spends the
    // tile's one line on both facts and truncates the caption after a couple
    // of words. Splitting them gives the caption the whole tile width, which
    // is the half the user actually distinguishes two Brave windows by. The
    // second line only exists when it carries something new: a caption that
    // already names its application ("Windows Explorer") renders as ONE line,
    // so the common case stays as quiet as Discord's.
    //
    // NEVER the geometry. It is in accessibleLabel() and nowhere else.
    function primaryLabel(row) {
        if (row === undefined || row === null)
            return "";
        var caption = row.name !== undefined ? String(row.name) : "";
        var appName = row.application !== undefined
            ? String(row.application) : "";
        if (!root.isWindowRow(row))
            return caption.length > 0
                ? caption
                // A display with no platform name still has to be nameable:
                // an unlabelled tile is a control the user cannot describe.
                : qsTr("Display %1").arg((row.index !== undefined
                                          ? row.index : 0) + 1);
        if (appName.length === 0)
            return caption.length > 0 ? caption : qsTr("Untitled window");
        if (caption.length === 0)
            return appName;
        // The caption already says which application it belongs to; repeating
        // it would be noise, so that window keeps one line.
        if (caption.toLowerCase().indexOf(appName.toLowerCase()) >= 0)
            return caption;
        return appName;
    }
    function secondaryLabel(row) {
        if (row === undefined || row === null)
            return "";
        if (root.isWindowRow(row)) {
            var caption = row.name !== undefined ? String(row.name) : "";
            return (caption.length > 0 && root.primaryLabel(row) !== caption)
                ? caption : "";
        }
        // Which screen this is, which is the only thing about a display the
        // preview cannot show you. Two facts, one line: the display the app is
        // on is the more useful of the two, so it wins.
        if (row.current === true)
            return qsTr("This screen");
        if (row.primary === true)
            return qsTr("Primary");
        return "";
    }
    /// Everything the tile says, PLUS the resolution — the one place it still
    /// belongs, because a screen reader has no preview to look at.
    function accessibleLabel(row) {
        var parts = [];
        var p = root.primaryLabel(row);
        if (p.length > 0)
            parts.push(p);
        var s = root.secondaryLabel(row);
        if (s.length > 0)
            parts.push(s);
        var g = (row !== undefined && row !== null
                 && row.geometry !== undefined) ? String(row.geometry) : "";
        if (g.length > 0)
            parts.push(g);
        return parts.join(", ");
    }

    // ---- selection --------------------------------------------------------

    function selectSource(idx) {
        if (idx < 0 || idx >= root.sources.length)
            return;
        root.selected = idx;
    }
    /// Move the highlight to a row of the VISIBLE tab, by view index.
    function moveToView(view) {
        var rows = root.rowsForTab(root.tab);
        if (view < 0 || view >= rows.length)
            return;
        root.selected = rows[view].sourceIndex;
        grid.positionViewAtIndex(view, GridView.Contain);
    }
    function moveSelection(delta) {
        var rows = root.rowsForTab(root.tab);
        if (rows.length === 0)
            return;
        var view = root.viewIndexOf(root.selected);
        view = view < 0 ? 0
                        : Math.max(0, Math.min(rows.length - 1, view + delta));
        root.moveToView(view);
    }
    /// The highlighted tile must always be the one Share would send, so a
    /// selection that is not in this tab moves to the first row that is.
    function selectionIntoView() {
        var rows = root.rowsForTab(root.tab);
        if (rows.length === 0 || root.viewIndexOf(root.selected) >= 0)
            return;
        root.selected = rows[0].sourceIndex;
    }
    /// Bring the tab and the selection into agreement after the list changes.
    ///
    /// The TAB FOLLOWS THE SELECTION rather than the other way round: the
    /// preselected row is the display the app is on, and opening on a tab
    /// where nothing is highlighted would leave Share sending something the
    /// user cannot see chosen.
    function normalize() {
        if (!root.sources || root.sources.length === 0)
            return;
        if (root.selected < 0 || root.selected >= root.sources.length)
            root.selected = 0;
        root.tab = root.isWindowRow(root.sources[root.selected])
            ? root.tabApplications : root.tabScreens;
        root.selectionIntoView();
    }

    onTabChanged: root.selectionIntoView()

    // Opened by the controller, never by the button: the button asks for a
    // share and the CONTROLLER decides whether a choice is needed — it starts
    // straight away when there is only one display, because a dialog to
    // confirm the only possible answer is a click that tells the user nothing.
    Connections {
        target: app.groupCall
        function onScreenShareSourcesAvailable() {
            root.selected = 0;
            // Preselect the display the app is on: it is what the user meant
            // often enough to be the right default, and it is what the
            // pre-picker behaviour did.
            for (var i = 0; i < root.sources.length; ++i) {
                if (root.sources[i].current) {
                    root.selected = i;
                    break;
                }
            }
            root.normalize();
            root.open();
        }
    }
    // The controller clears the list when the share starts or is abandoned,
    // so a dialog left open by any other path closes with it rather than
    // sitting over a call it can no longer act on.
    onSourcesChanged: {
        if (root.visible && root.sources.length === 0) {
            root.close();
            return;
        }
        root.normalize();
    }

    /// Set for the one frame between pressing Share and the controller
    /// clearing the list.
    ///
    /// Without it `onClosed` fires while the sources are still populated,
    /// cancels the selection, and the `chooseScreenShareSource` call that
    /// follows finds an empty list and returns — a Share button that closes
    /// the dialog and shares nothing.
    property bool accepting: false
    /// One confirmation per opening. Return reaches confirmShare() twice —
    /// once through the grid's own handler and once through QQuickDialog's
    /// accept() — and the second call would index into a list the controller
    /// has already cleared.
    property bool confirmed: false

    function confirmShare() {
        if (root.confirmed)
            return;
        if (root.selected < 0 || root.selected >= root.sources.length)
            return;
        // The same gate as the button, because Return reaches here without
        // passing through it.
        if (root.viewIndexOf(root.selected) < 0)
            return;
        var chosen = root.selected;
        root.confirmed = true;
        root.accepting = true;
        root.close();
        if (app.groupCall)
            app.groupCall.chooseScreenShareSource(chosen);
    }

    onAboutToShow: {
        root.accepting = false;
        root.confirmed = false;
        // A stale Accepted from the previous opening would suppress the
        // cancel below on this one.
        root.result = Dialog.Rejected;
    }
    onOpened: grid.forceActiveFocus()
    onAccepted: root.confirmShare()
    onRejected: {
        if (app.groupCall)
            app.groupCall.cancelScreenShareSelection();
    }
    onClosed: {
        // `result` covers QQuickDialog's OWN accept path: Return makes it call
        // accept(), which CLOSES before it emits accepted(), so `accepting` is
        // still false here and the cancel would clear the very list
        // confirmShare() is about to read.
        if (!root.accepting && root.result !== Dialog.Accepted
                && root.sources.length > 0 && app.groupCall)
            app.groupCall.cancelScreenShareSelection();
        root.accepting = false;
    }

    // ---- tile geometry ----------------------------------------------------
    //
    // Derived, never literal, so a 140% text scale grows the tiles with the
    // labels instead of squeezing them. Every value below reads the GRID's
    // width, which comes from the dialog's own fixed width — nothing here
    // reads a size this layout computes from its children.
    readonly property int tileGap: AppTheme.spacing8
    readonly property int minTileWidth: AppTheme.scaled(200)
    readonly property int gridColumns:
        grid.width > 0
            ? Math.max(1, Math.min(3,
                                   Math.floor(grid.width / root.minTileWidth)))
            : 1
    readonly property int cellW:
        grid.width > 0 ? Math.floor(grid.width / root.gridColumns)
                       : root.minTileWidth
    readonly property int previewW: Math.max(1, root.cellW - 2 * root.tileGap)
    readonly property int previewH: Math.round(root.previewW * 9 / 16)
    /// The button's own padding (top and bottom), the 16:9 picture, the three
    /// gaps its content column leaves between four children, and one line
    /// each of the two labels. Written out rather than rounded up to a
    /// literal: a cell that is a few pixels short does not clip visibly, it
    /// makes the LAYOUT shrink the caption, which reads as a font bug.
    readonly property int cellH:
        2 * root.tileGap + root.previewH + 3 * AppTheme.spacing4
        + Math.ceil(titleMetrics.height) + Math.ceil(metaMetrics.height)
    /// Twice the tile, so the grab still looks like the window at a 125-200%
    /// device pixel ratio. Quantised to 64px steps because `sourceSize` is
    /// part of the image's identity: without it, dragging the window edge
    /// would re-grab every desktop in the grid on every pixel of the resize.
    /// The provider caps at 640 on the long edge regardless.
    readonly property int previewPixelWidth:
        Math.min(640, Math.max(128, Math.ceil(root.previewW * 2 / 64) * 64))
    readonly property int gridRows:
        Math.max(1, Math.ceil(root.shownRows.length / root.gridColumns))
    readonly property int maxGridHeight: {
        var available = root.parent ? root.parent.height : 800;
        // Room for the title, the hint, the tabs and the buttons.
        return Math.max(root.cellH, available - AppTheme.scaled(260));
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // Non-visual, so the layout never sees them: the tile height has to be
        // known before a delegate exists, and it must come from the FONT
        // rather than from a guessed line height, or a different UI face
        // clips the caption.
        // FontMetrics, not TextMetrics: the question is the FONT's line
        // height, and TextMetrics can only answer it by being handed a sample
        // string — which is then a `text:` property holding untranslated
        // words, and the localization scan is right to refuse one.
        FontMetrics {
            id: titleMetrics
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
            font.weight: AppTheme.weightStrong
        }
        FontMetrics {
            id: metaMetrics
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            // Says what each choice actually shares, because "share my screen"
            // and "share this window" have different consequences and the
            // difference is the whole reason to offer both.
            text: root.windowCount === 0
                ? qsTr("Everyone in the call sees the whole display you pick.")
                : qsTr("A screen shares everything on it. A window shares only that window, even if something is in front of it.")
        }

        SegmentedControl {
            id: tabs
            objectName: "shareTabs"
            Layout.fillWidth: true
            storm: root.storm
            current: root.tab
            // An empty tab stays offered and says so in its own words — see
            // the note on the tab properties above.
            model: [
                { label: qsTr("Applications"), value: root.tabApplications },
                { label: qsTr("Screens"), value: root.tabScreens }
            ]
            onActivated: (value) => {
                root.tab = value;
                // Keep the arrows working after a tab is clicked: the segment
                // takes focus, and the grid is what reads them.
                grid.forceActiveFocus();
            }
        }

        // The grid sits in a plain Item so the empty-tab message can be
        // centred over it WITHOUT becoming a child of the Flickable's content
        // (where it would scroll away from the middle).
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight:
                Math.min(root.maxGridHeight, root.gridRows * root.cellH)

            GridView {
                id: grid
                // Named so a test can find the grid and press a real tile; the
                // grouped-list rework shipped with its rows unclickable
                // precisely because nothing could reach one.
                objectName: "sourceGrid"
                anchors.fill: parent
                clip: true
                model: root.shownRows
                cellWidth: root.cellW
                cellHeight: root.cellH
                cacheBuffer: root.cellH * 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                // The HIGHLIGHT IS `root.selected`, not currentIndex. A JS
                // array model resets whenever the tab changes, and an item
                // view clamps its own currentIndex across a reset — which
                // would move the selection to a row nobody chose. Arrow keys
                // are handled here instead, against the visible tab's rows.
                keyNavigationEnabled: false
                ScrollBar.vertical: AppScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                SmoothWheelArea {}

                Keys.onLeftPressed: (event) => {
                    root.moveSelection(-1);
                    event.accepted = true;
                }
                Keys.onRightPressed: (event) => {
                    root.moveSelection(1);
                    event.accepted = true;
                }
                Keys.onUpPressed: (event) => {
                    root.moveSelection(-root.gridColumns);
                    event.accepted = true;
                }
                Keys.onDownPressed: (event) => {
                    root.moveSelection(root.gridColumns);
                    event.accepted = true;
                }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                        root.confirmShare();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Home) {
                        root.moveToView(0);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_End) {
                        root.moveToView(root.rowsForTab(root.tab).length - 1);
                        event.accepted = true;
                    }
                }

                delegate: AbstractButton {
                    id: tile
                    required property var modelData
                    required property int index

                    // The entry's OWN index in the unfiltered `sources`,
                    // carried by the model rather than derived from `index`.
                    readonly property var row: modelData.source
                    readonly property int sourceIndex: modelData.sourceIndex
                    readonly property bool chosen:
                        root.selected === tile.sourceIndex

                    width: grid.cellWidth
                    height: grid.cellHeight
                    padding: root.tileGap
                    hoverEnabled: true
                    // The GRID owns keyboard focus for the whole tab; a tile
                    // that could take it would break arrow navigation.
                    focusPolicy: Qt.NoFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.checkable: true
                    Accessible.checked: tile.chosen
                    Accessible.name: root.accessibleLabel(tile.row)
                    onClicked: {
                        root.selectSource(tile.sourceIndex);
                        grid.forceActiveFocus();
                    }

                    background: Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: AppTheme.radiusLg
                        color: (tile.hovered && !tile.chosen)
                            ? AppTheme.stormSelection : "transparent"
                    }

                    contentItem: ColumnLayout {
                        spacing: AppTheme.spacing4

                        // THE PREVIEW, which is now the tile. The accent frame
                        // is on the picture rather than the whole cell so the
                        // selection reads at a glance in a grid of pictures.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.previewH
                            radius: AppTheme.radiusMd
                            // A dark plate, so a window that is not 16:9
                            // letterboxes instead of being stretched into
                            // something the user cannot recognise.
                            color: AppTheme.stormInset
                            border.width: tile.chosen ? 2 : 1
                            border.color: tile.chosen ? AppTheme.accent
                                                      : AppTheme.stormBorder
                            clip: true

                            Icon {
                                anchors.centerIn: parent
                                // BOTH names checked against Icon.qml's map.
                                // The bundled font is a SUBSET and an unmapped
                                // name renders as tofu; `web_asset`, the
                                // obvious glyph for a window, is not in it.
                                name: root.isWindowRow(tile.row)
                                    ? "fit_screen" : "screen_share"
                                size: AppTheme.scaled(28)
                                color: tile.chosen ? AppTheme.accent
                                                   : AppTheme.stormTextMuted
                            }

                            Image {
                                id: preview
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false   // a live grab; a cached one lies
                                sourceSize.width: root.previewPixelWidth
                                // The id carries WHICH thing, in the same
                                // terms the controller uses to start the
                                // capture — a window handle or the row's
                                // display index — so a tile cannot preview
                                // something other than what pressing Share
                                // would send.
                                source: root.isWindowRow(tile.row)
                                    ? "image://lightning-sharesource/w"
                                      + tile.row.windowHandle
                                    : "image://lightning-sharesource/s"
                                      + (tile.row.index !== undefined
                                         ? tile.row.index : 0)
                                // Null image (a window that closed between
                                // being listed and being drawn, or any
                                // platform without previews) leaves the glyph
                                // showing. That is a legitimate answer, not a
                                // failure.
                                visible: status === Image.Ready
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Never empty: primaryLabel() names an unnamed row
                            // rather than returning "", which also keeps this
                            // out of the never-laid-out empty-Text hazard.
                            text: root.primaryLabel(tile.row)
                            color: tile.chosen ? AppTheme.stormText
                                               : AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            font.weight: AppTheme.weightStrong
                        }
                        // A Loader, not a Label with an empty string: a
                        // never-laid-out Text keeps ItemObservesViewport for
                        // the life of the delegate, and this one lives in a
                        // scrolling view (see CLAUDE.md §16).
                        Loader {
                            Layout.fillWidth: true
                            active: root.secondaryLabel(tile.row).length > 0
                            sourceComponent: Label {
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                text: root.secondaryLabel(tile.row)
                                color: AppTheme.stormTextMuted
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        // Soaks up the rounding slack in the fixed cell height
                        // so the picture and its label stay packed to the top.
                        Item {
                            Layout.fillHeight: true
                            Layout.preferredHeight: 0
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 2 * AppTheme.spacing20
                visible: root.shownRows.length === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                // A STATEMENT ABOUT LIGHTNING, NOT ABOUT THE DESKTOP, when
                // Lightning is the reason the list is empty. Window
                // enumeration is Windows-only, so on macOS "No open windows
                // to share" is simply false in front of a user with three
                // apps running.
                text: root.tab !== root.tabApplications
                    ? qsTr("No screens were found.")
                    : (root.windowCaptureSupported
                       ? qsTr("No open windows to share.")
                       : qsTr("Sharing a single window isn't available on this "
                              + "platform yet. Pick a screen instead."))
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: root.storm
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            AppButton {
                objectName: "shareConfirmButton"
                storm: root.storm
                kind: "primary"
                text: qsTr("Share")
                // IN THE VISIBLE TAB, not merely in the array. A tab with
                // no rows leaves `selected` pointing into the OTHER tab, so
                // bounds-checking the array alone offers Share over an empty
                // grid with nothing highlighted — and pressing it sends
                // whatever the other tab had chosen. That is the ordinary
                // macOS path, not a corner case: window enumeration is
                // Windows-only, so Applications is permanently empty there.
                enabled: root.viewIndexOf(root.selected) >= 0
                onClicked: root.confirmShare()
            }
        }
    }
}
