import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Choose what to share on platforms without a portal. On Linux the xdg portal
// shows its own picker and hands back a PipeWire node, so this stays closed; it
// opens on a Linux desktop without a portal (SfuCallController::
// linuxShareRoute lists displays itself). Windows and macOS have no broker, so
// the capture element needs a display index or window handle chosen here. A
// grid of preview tiles, with Screens and Applications as tabs. Captions lead
// with the application, since a browser caption is the tab's title. The
// resolution appears only in the accessible name.
AppDialog {
    id: root

    title: qsTr("Choose what to share")
    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    // A ceiling that keeps the dialog inside a small window; everything inside
    // elides or wraps.
    width: Math.min(920, parent ? parent.width - 64 : 920)

    /// Bound to the live call and not readonly, so tests can supply rows to
    /// press.
    property var sources: app.groupCall ? app.groupCall.screenShareSources : []

    /// An index into `sources`, which chooseScreenShareSource(index) indexes
    /// unfiltered. Each tab entry carries its `sourceIndex` (see rowsForTab), so
    /// the mapping is never recomputed at the point of use.
    property int selected: 0

    // A row is a window when it has a non-zero handle, the same fact the
    // controller keys capture off.
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

    // Tabs. Both are always offered; an empty one says so, so things do not
    // move around with the machine's state.
    readonly property string tabApplications: "applications"
    readonly property string tabScreens: "screens"
    property string tab: tabScreens

    /// The rows of one tab, each carrying its index in `sources`.
    function rowsForTab(which) {
        var out = [];
        for (var i = 0; i < sources.length; ++i) {
            if (isWindowRow(sources[i]) === (which === root.tabApplications))
                out.push({ source: sources[i], sourceIndex: i });
        }
        return out;
    }
    readonly property var shownRows: root.rowsForTab(root.tab)
    /// Whether this build can list windows at all, distinct from whether any are
    /// open.
    readonly property bool windowCaptureSupported:
        app.groupCall ? app.groupCall.windowCaptureSupported : false

    /// Where `sources[idx]` sits in the visible tab, or -1. Computed via
    /// rowsForTab rather than read from `shownRows`: a `tab` change handler can
    /// run before that binding updates, which would highlight the wrong row and
    /// share something not shown as selected.
    function viewIndexOf(idx) {
        var rows = root.rowsForTab(root.tab);
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].sourceIndex === idx)
                return i;
        return -1;
    }

    // Labelling: application first, caption on its own line (only when it adds
    // something), so the caption gets the full tile width. Never the geometry,
    // which is only in accessibleLabel().
    function primaryLabel(row) {
        if (row === undefined || row === null)
            return "";
        var caption = row.name !== undefined ? String(row.name) : "";
        var appName = row.application !== undefined
            ? String(row.application) : "";
        if (!root.isWindowRow(row))
            return caption.length > 0
                ? caption
                // A display with no platform name still needs a name.
                : qsTr("Display %1").arg((row.index !== undefined
                                          ? row.index : 0) + 1);
        if (appName.length === 0)
            return caption.length > 0 ? caption : qsTr("Untitled window");
        if (caption.length === 0)
            return appName;
        // The caption already names its application; keep one line.
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
        // Whether this is the display the app is on; the preview cannot show
        // that.
        if (row.current === true)
            return qsTr("This screen");
        if (row.primary === true)
            return qsTr("Primary");
        return "";
    }
    /// Everything the tile says, plus the resolution, for screen readers.
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

    // Selection

    function selectSource(idx) {
        if (idx < 0 || idx >= root.sources.length)
            return;
        root.selected = idx;
    }
    /// Move the highlight to a row of the visible tab, by view index.
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
    /// The highlighted tile must be what Share would send.
    function selectionIntoView() {
        var rows = root.rowsForTab(root.tab);
        if (rows.length === 0 || root.viewIndexOf(root.selected) >= 0)
            return;
        root.selected = rows[0].sourceIndex;
    }
    /// Bring tab and selection into agreement after the list changes. The tab
    /// follows the selection, which starts on the display the app is on.
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

    // Opened by the controller, which starts immediately when there is only one
    // display.
    Connections {
        target: app.groupCall
        function onScreenShareSourcesAvailable() {
            root.selected = 0;
            // Preselect the display the app is on.
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
    // The controller clears the list when the share starts or is abandoned;
    // close with it.
    onSourcesChanged: {
        if (root.visible && root.sources.length === 0) {
            root.close();
            return;
        }
        root.normalize();
    }

    /// Set for the frame between pressing Share and the controller clearing the
    /// list; otherwise onClosed would cancel first and Share would share
    /// nothing.
    property bool accepting: false
    /// One confirmation per opening: Return reaches confirmShare() twice (the
    /// grid's handler and QQuickDialog's accept()).
    property bool confirmed: false

    function confirmShare() {
        if (root.confirmed)
            return;
        if (root.selected < 0 || root.selected >= root.sources.length)
            return;
        // Same gate as the button; Return bypasses it.
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
        // Clear a stale Accepted from the previous opening.
        root.result = Dialog.Rejected;
    }
    onOpened: grid.forceActiveFocus()
    onAccepted: root.confirmShare()
    onRejected: {
        if (app.groupCall)
            app.groupCall.cancelScreenShareSelection();
    }
    onClosed: {
        // `result` covers QQuickDialog's own accept path: Return closes before
        // accepted() is emitted, so `accepting` is still false here.
        if (!root.accepting && root.result !== Dialog.Accepted
                && root.sources.length > 0 && app.groupCall)
            app.groupCall.cancelScreenShareSelection();
        root.accepting = false;
    }

    // Tile geometry, derived from the grid's width (fixed by the dialog), so
    // text scaling grows tiles with their labels and nothing reads a
    // child-computed size.
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
    /// Button padding, the 16:9 picture, three gaps and one line of each label.
    /// Written out: a short cell makes the layout shrink the caption.
    readonly property int cellH:
        2 * root.tileGap + root.previewH + 3 * AppTheme.spacing4
        + Math.ceil(titleMetrics.height) + Math.ceil(metaMetrics.height)
    /// Twice the tile for high-DPI, quantised to 64px because sourceSize is part
    /// of the image's identity (otherwise every resize pixel would re-grab every
    /// desktop). The provider caps at 640.
    readonly property int previewPixelWidth:
        Math.min(640, Math.max(128, Math.ceil(root.previewW * 2 / 64) * 64))
    readonly property int gridRows:
        Math.max(1, Math.ceil(root.shownRows.length / root.gridColumns))
    readonly property int maxGridHeight: {
        var available = root.parent ? root.parent.height : 800;
        // Room for the title, hint, tabs and buttons.
        return Math.max(root.cellH, available - AppTheme.scaled(260));
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // FontMetrics (non-visual, font-derived) for the tile height, which
        // must be known before any delegate exists. TextMetrics would need an
        // untranslated sample string.
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
            // Says what each choice shares.
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
            // An empty tab stays offered and says so.
            model: [
                { label: qsTr("Applications"), value: root.tabApplications },
                { label: qsTr("Screens"), value: root.tabScreens }
            ]
            onActivated: (value) => {
                root.tab = value;
                // Keep the arrow keys working after a tab click.
                grid.forceActiveFocus();
            }
        }

        // A plain Item so the empty-tab message is centred over the grid rather
        // than scrolling inside it.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight:
                Math.min(root.maxGridHeight, root.gridRows * root.cellH)

            GridView {
                id: grid
                // Named so a test can press a real tile.
                objectName: "sourceGrid"
                anchors.fill: parent
                clip: true
                model: root.shownRows
                cellWidth: root.cellW
                cellHeight: root.cellH
                cacheBuffer: root.cellH * 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                // The highlight is `root.selected`, not currentIndex: the JS
                // array model resets on tab change and the view would clamp
                // currentIndex to a row nobody chose. Arrow keys are handled
                // here.
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

                    // The entry's own index in the unfiltered `sources`.
                    readonly property var row: modelData.source
                    readonly property int sourceIndex: modelData.sourceIndex
                    readonly property bool chosen:
                        root.selected === tile.sourceIndex

                    width: grid.cellWidth
                    height: grid.cellHeight
                    padding: root.tileGap
                    hoverEnabled: true
                    // The grid owns keyboard focus; a focusable tile would
                    // break arrow keys.
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

                        // The preview is the tile; the accent frame is on the
                        // picture.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.previewH
                            radius: AppTheme.radiusMd
                            // A dark plate, so a non-16:9 window letterboxes
                            // instead of stretching.
                            color: AppTheme.stormInset
                            border.width: tile.chosen ? 2 : 1
                            border.color: tile.chosen ? AppTheme.accent
                                                      : AppTheme.stormBorder
                            clip: true

                            Icon {
                                anchors.centerIn: parent
                                // Both names exist in Icon.qml's map; the icon
                                // font is a subset.
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
                                // The id names the thing in the controller's
                                // own terms (window handle or display index),
                                // so the preview matches what Share would send.
                                source: root.isWindowRow(tile.row)
                                    ? "image://lightning-sharesource/w"
                                      + tile.row.windowHandle
                                    : "image://lightning-sharesource/s"
                                      + (tile.row.index !== undefined
                                         ? tile.row.index : 0)
                                // A null image (window closed, or no previews
                                // on this platform) leaves the glyph showing.
                                visible: status === Image.Ready
                            }
                        }

                        Label {
                            // Untrusted text: never markup.
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Never empty: primaryLabel() names unnamed rows.
                            text: root.primaryLabel(tile.row)
                            color: tile.chosen ? AppTheme.stormText
                                               : AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            font.weight: AppTheme.weightStrong
                        }
                        // A Loader: an empty Text never laid out stays a
                        // viewport observer.
                        Loader {
                            Layout.fillWidth: true
                            active: root.secondaryLabel(tile.row).length > 0
                            sourceComponent: Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                text: root.secondaryLabel(tile.row)
                                color: AppTheme.stormTextMuted
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        // Takes up rounding slack so content stays packed to
                        // the top.
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
                // When Lightning is why the list is empty (window enumeration
                // is Windows-only), say that rather than claiming there are no
                // windows.
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
            // Share audio is absent, not disabled, where the build cannot
            // capture playback (answered by GStreamer at runtime, since the
            // plugins shipped are a packaging fact). Quality settings live
            // here, where you share: both drive encoder cost (CPU downscale
            // plus VP8 encode competing with what is being shared).
            Label {
                text: qsTr("Quality")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
            AppComboBox {
                id: shareResCombo
                objectName: "shareResolutionCombo"
                storm: root.storm
                implicitWidth: 110
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("720p"), value: 720 },
                    { label: qsTr("1080p"), value: 1080 },
                    { label: qsTr("1440p"), value: 1440 },
                    { label: qsTr("4K"), value: 2160 }
                ]
                Accessible.name: qsTr("Screen share resolution")
                // Never bind currentIndex: indexOfValue() is -1 at creation.
                // The model's labels are qsTr(), so a language change re-syncs
                // AppComboBox to its last synced value; same fix as
                // wheelSpeedCombo in SettingsScreen.
                Component.onCompleted: syncToValue(app.settings.shareMaxHeight)
                onActivated: {
                    app.settings.shareMaxHeight = currentValue;
                    syncToValue(app.settings.shareMaxHeight);
                }
                Connections {
                    target: app.settings
                    function onShareQualityChanged() {
                        shareResCombo.syncToValue(app.settings.shareMaxHeight);
                    }
                }
            }
            AppComboBox {
                id: shareFpsCombo
                objectName: "shareFpsCombo"
                storm: root.storm
                implicitWidth: 100
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("15 fps"), value: 15 },
                    { label: qsTr("30 fps"), value: 30 },
                    { label: qsTr("60 fps"), value: 60 }
                ]
                Accessible.name: qsTr("Screen share frame rate")
                Component.onCompleted: syncToValue(app.settings.shareFps)
                onActivated: {
                    app.settings.shareFps = currentValue;
                    syncToValue(app.settings.shareFps);
                }
                Connections {
                    target: app.settings
                    function onShareQualityChanged() {
                        shareFpsCombo.syncToValue(app.settings.shareFps);
                    }
                }
            }
            Label {
                objectName: "shareQualityWarning"
                visible: app.settings.shareQualityDemanding
                text: qsTr("slow")
                color: AppTheme.warning
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
            CheckBox {
                objectName: "shareAudioCheck"
                visible: app.groupCall && app.groupCall.shareAudioSupported
                text: qsTr("Share audio")
                checked: app.groupCall ? app.groupCall.shareAudioEnabled
                                       : false
                onToggled: {
                    if (app.groupCall)
                        app.groupCall.shareAudioEnabled = checked;
                    // Restore the binding: a click writes `checked` and
                    // destroys it, and this must stay in sync with the call
                    // bar's menu.
                    checked = Qt.binding(function () {
                        return app.groupCall ? app.groupCall.shareAudioEnabled
                                             : false;
                    });
                }
                ToolTip.visible: hovered
                // Says what is actually captured. With per-application capture
                // (Linux with PipeWire) Lightning's own playback is excluded;
                // phrased as intent because a daemon restart can still send the
                // share down the monitor fallback. With only the output monitor
                // (post-mix), the call's audio cannot be subtracted (see
                // docs/voice-calls.md); routing Lightning to another output
                // device is the workaround.
                ToolTip.text: (app.groupCall
                               && app.groupCall.shareAudioExcludesOwnPlayback)
                    ? qsTr("Send what this computer is playing, alongside "
                           + "the picture. Where this system allows it, "
                           + "Lightning's own audio is left out so the "
                           + "others do not hear themselves.")
                    : qsTr("Send what this computer is playing, alongside "
                           + "the picture. On this system that includes the "
                           + "call itself, so others hear themselves unless "
                           + "Lightning's audio plays on a different output "
                           + "device.")
            }
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
                // In the visible tab, not merely in the array: an empty tab
                // leaves `selected` pointing into the other one (Applications
                // is always empty on macOS).
                enabled: root.viewIndexOf(root.selected) >= 0
                onClicked: root.confirmShare()
            }
        }
    }
}
