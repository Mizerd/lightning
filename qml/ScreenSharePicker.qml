import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

AppDialog {
    id: root

    title: qsTr("Choose what to share")
    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(920, parent ? parent.width - 64 : 920)

    property var sources: app.groupCall ? app.groupCall.screenShareSources : []

    property int selected: 0

    function isWindowRow(row) {
        return row !== undefined && row !== null
            && row.windowHandle !== undefined && row.windowHandle !== 0;
    }
    function shareSourceId(row) {
        return root.isWindowRow(row)
            ? "w" + row.windowHandle
            : "s" + (row.index !== undefined ? row.index : 0);
    }
    readonly property int screenCount: {
        var n = 0;
        for (var i = 0; i < sources.length; ++i)
            if (!isWindowRow(sources[i]))
                ++n;
        return n;
    }
    readonly property int windowCount: sources.length - screenCount

    readonly property string tabApplications: "applications"
    readonly property string tabScreens: "screens"
    property string tab: tabScreens

    function rowsForTab(which) {
        var out = [];
        for (var i = 0; i < sources.length; ++i) {
            if (isWindowRow(sources[i]) === (which === root.tabApplications))
                out.push({ source: sources[i], sourceIndex: i });
        }
        return out;
    }
    readonly property var shownRows: root.rowsForTab(root.tab)
    readonly property bool windowCaptureSupported:
        app.groupCall ? app.groupCall.windowCaptureSupported : false

    function viewIndexOf(idx) {
        var rows = root.rowsForTab(root.tab);
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].sourceIndex === idx)
                return i;
        return -1;
    }

    function primaryLabel(row) {
        if (row === undefined || row === null)
            return "";
        var caption = row.name !== undefined ? String(row.name) : "";
        var appName = row.application !== undefined
            ? String(row.application) : "";
        if (!root.isWindowRow(row))
            return caption.length > 0
                ? caption
                : qsTr("Display %1").arg((row.index !== undefined
                                          ? row.index : 0) + 1);
        if (appName.length === 0)
            return caption.length > 0 ? caption : qsTr("Untitled window");
        if (caption.length === 0)
            return appName;
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
        if (row.current === true)
            return qsTr("This screen");
        if (row.primary === true)
            return qsTr("Primary");
        return "";
    }
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

    function selectSource(idx) {
        if (idx < 0 || idx >= root.sources.length)
            return;
        root.selected = idx;
    }
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
    function selectionIntoView() {
        var rows = root.rowsForTab(root.tab);
        if (rows.length === 0 || root.viewIndexOf(root.selected) >= 0)
            return;
        root.selected = rows[0].sourceIndex;
    }
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

    readonly property bool canChooseApps:
        !!app.groupCall && app.groupCall.shareAudioCanChooseApps
        && app.groupCall.shareAudioSupported
    property bool soundTouched: false
    property bool soundAutoChosen: false
    property int soundSavedMode: 1
    property var soundSavedApps: []
    readonly property var selectedRow:
        (root.selected >= 0 && root.selected < root.sources.length)
            ? root.sources[root.selected] : null
    readonly property string selectedWindowAudioKey:
        (root.selectedRow && root.isWindowRow(root.selectedRow)
         && root.selectedRow.audioAppKey !== undefined)
            ? String(root.selectedRow.audioAppKey) : ""
    function restoreAutoSound() {
        if (!root.soundAutoChosen || !app.groupCall)
            return;
        root.soundAutoChosen = false;
        app.groupCall.restoreShareAudioChoice(root.soundSavedMode,
                                              root.soundSavedApps);
    }
    readonly property bool selectedWindowContainsUs:
        !!root.selectedRow && root.isWindowRow(root.selectedRow)
        && root.selectedRow.audioAppContainsUs === true
    readonly property bool selectedWindowHasSession: {
        var key = root.selectedWindowAudioKey;
        if (key.length === 0 || !app.groupCall)
            return false;
        var rows = app.groupCall.shareAudioApplications;
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].key === key && rows[i].session === true
                    && rows[i].containsUs !== true)
                return true;
        }
        return false;
    }
    function followSelectionWithSound() {
        if (!root.visible || root.soundTouched || !root.canChooseApps
                || !app.groupCall)
            return;
        var row = root.selectedRow;
        if (row && root.isWindowRow(row) && root.selectedWindowAudioKey.length > 0
                && !root.selectedWindowContainsUs && root.selectedWindowHasSession
                && app.groupCall.shareAudioMode !== 0) {
            if (!root.soundAutoChosen) {
                root.soundSavedMode = app.groupCall.shareAudioMode;
                root.soundSavedApps = app.groupCall.shareAudioApps.slice();
                root.soundAutoChosen = true;
            }
            app.groupCall.chooseOnlyShareAudioApp(
                root.selectedWindowAudioKey,
                row.application !== undefined ? String(row.application) : "");
        } else {
            root.restoreAutoSound();
        }
    }
    onSelectedChanged: root.followSelectionWithSound()

    Connections {
        target: app.groupCall
        function onScreenShareSourcesAvailable() {
            root.selected = 0;
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
    onSourcesChanged: {
        if (root.visible && root.sources.length === 0) {
            root.close();
            return;
        }
        root.normalize();
        root.followSelectionWithSound();
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
        if (root.viewIndexOf(root.selected) < 0)
            return;
        var chosen = root.selected;
        root.confirmed = true;
        root.accepting = true;
        // A window's application chosen for this share is put back when the
        // share ends, not now (the share needs it) and not never.
        if (root.soundAutoChosen && app.groupCall) {
            app.groupCall.restoreShareAudioChoiceAfterShare(
                root.soundSavedMode, root.soundSavedApps);
            root.soundAutoChosen = false;
        }
        root.close();
        if (app.groupCall)
            app.groupCall.chooseScreenShareSource(chosen);
    }

    property bool watchingApps: false
    function setWatchingApps(on) {
        if (!app.groupCall || on === root.watchingApps)
            return;
        root.watchingApps = on;
        app.groupCall.watchShareAudioApplications(on);
    }
    Connections {
        target: app.groupCall
        function onShareAudioApplicationsChanged() {
            root.followSelectionWithSound();
        }
    }
    Component.onDestruction: {
        if (root.watchingApps && app.groupCall)
            app.groupCall.watchShareAudioApplications(false);
        root.watchingApps = false;
    }

    onAboutToShow: {
        if (app.groupCall)
            app.groupCall.refreshShareAudioCapability();
        root.accepting = false;
        root.confirmed = false;
        root.soundTouched = false;
        root.soundAutoChosen = false;
        root.result = Dialog.Rejected;
    }
    onOpened: {
        grid.forceActiveFocus();
        root.setWatchingApps(root.canChooseApps && root.windowCaptureSupported);
        root.followSelectionWithSound();
    }
    onAccepted: root.confirmShare()
    onRejected: {
        root.restoreAutoSound();
        if (app.groupCall)
            app.groupCall.cancelScreenShareSelection();
    }
    onClosed: {
        root.setWatchingApps(false);
        if (!root.accepting && root.result !== Dialog.Accepted) {
            root.restoreAutoSound();
            if (root.sources.length > 0 && app.groupCall)
                app.groupCall.cancelScreenShareSelection();
        }
        root.accepting = false;
    }

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
    readonly property int cellH:
        2 * root.tileGap + root.previewH + 3 * AppTheme.spacing4
        + Math.ceil(titleMetrics.height) + Math.ceil(metaMetrics.height)
    readonly property int previewPixelWidth:
        Math.min(640, Math.max(128, Math.ceil(root.previewW * 2 / 64) * 64))
    readonly property int gridRows:
        Math.max(1, Math.ceil(root.shownRows.length / root.gridColumns))
    readonly property int maxGridHeight: {
        var available = root.parent ? root.parent.height : 800;
        return Math.max(root.cellH, available - AppTheme.scaled(260)
                                    - Math.ceil(soundSection.implicitHeight));
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

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
            model: [
                { label: qsTr("Applications"), value: root.tabApplications },
                { label: qsTr("Screens"), value: root.tabScreens }
            ]
            onActivated: (value) => {
                root.tab = value;
                grid.forceActiveFocus();
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight:
                Math.min(root.maxGridHeight, root.gridRows * root.cellH)

            GridView {
                id: grid
                objectName: "sourceGrid"
                anchors.fill: parent
                clip: true
                model: root.shownRows
                cellWidth: root.cellW
                cellHeight: root.cellH
                cacheBuffer: root.cellH * 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
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

                    readonly property var row: modelData.source
                    readonly property int sourceIndex: modelData.sourceIndex
                    readonly property bool chosen:
                        root.selected === tile.sourceIndex

                    width: grid.cellWidth
                    height: grid.cellHeight
                    padding: root.tileGap
                    hoverEnabled: true
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

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.previewH
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.width: tile.chosen ? 2 : 1
                            border.color: tile.chosen ? AppTheme.accent
                                                      : AppTheme.stormBorder
                            clip: true

                            Icon {
                                anchors.centerIn: parent
                                name: root.isWindowRow(tile.row)
                                    ? "fit_screen" : "screen_share"
                                size: AppTheme.scaled(28)
                                color: tile.chosen ? AppTheme.accent
                                                   : AppTheme.stormTextMuted
                            }

                            Image {
                                id: preview
                                objectName: "sharePickerPreview_"
                                            + root.shareSourceId(tile.row)
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false
                                sourceSize.width: root.previewPixelWidth
                                // ShareSourceImageProvider always answers null
                                // off Windows (see its header — Linux uses the
                                // portal's own picker and never reaches this
                                // dialog with a real screen; macOS lists
                                // displays only), so asking it there bought
                                // nothing but a "Failed to get image from
                                // provider" warning on every tile, every open.
                                // Only ask where an answer is possible.
                                source: Qt.platform.os === "windows"
                                    ? "image://lightning-sharesource/"
                                      + root.shareSourceId(tile.row)
                                    : ""
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
                            text: root.primaryLabel(tile.row)
                            color: tile.chosen ? AppTheme.stormText
                                               : AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            font.weight: AppTheme.weightStrong
                        }
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
                text: root.tab !== root.tabApplications
                    ? qsTr("No screens were found.")
                    : (root.windowCaptureSupported
                       ? qsTr("No open windows to share.")
                       : qsTr("Sharing a single window isn't available on this "
                              + "platform yet. Pick a screen instead."))
            }
        }

        ColumnLayout {
            id: soundSection
            objectName: "shareAudioSection"
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            visible: !!app.groupCall

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    text: qsTr("Sound")
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
                AppComboBox {
                    id: shareAudioCombo
                    objectName: "shareAudioCombo"
                    enabled: !!app.groupCall && app.groupCall.shareAudioSupported
                    restoreAfterModelReset: false
                    storm: root.storm
                    implicitWidth: 190
                    textRole: "label"
                    valueRole: "value"
                    model: root.canChooseApps
                        ? [
                            { label: qsTr("No sound"), value: 0 },
                            { label: qsTr("Entire system"), value: 1 },
                            { label: qsTr("Only chosen apps"), value: 2 }
                          ]
                        : [
                            { label: qsTr("No sound"), value: 0 },
                            { label: qsTr("Entire system"), value: 1 }
                          ]
                    Accessible.name: qsTr("Screen share sound")
                    Component.onCompleted: syncToValue(
                        app.groupCall ? app.groupCall.shareAudioMode : 0)
                    onModelChanged: syncToValue(
                        app.groupCall ? app.groupCall.shareAudioMode : 0)
                    onActivated: {
                        root.soundTouched = true;
                        root.soundAutoChosen = false;
                        if (app.groupCall)
                            app.groupCall.shareAudioMode = currentValue;
                        syncToValue(app.groupCall
                                    ? app.groupCall.shareAudioMode : 0);
                    }
                    Connections {
                        target: app.groupCall
                        function onMediaStateChanged() {
                            shareAudioCombo.syncToValue(
                                app.groupCall.shareAudioMode);
                        }
                    }
                }
                Label {
                    objectName: "shareAudioStatus"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: app.groupCall ? app.groupCall.shareAudioStatus : ""
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
            }

            Label {
                objectName: "shareAudioWindowUnmatched"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                visible: root.canChooseApps && !!root.selectedRow
                         && root.isWindowRow(root.selectedRow)
                         && !root.soundTouched
                         && (root.selectedWindowContainsUs
                             || root.selectedWindowAudioKey.length === 0
                             || !root.selectedWindowHasSession)
                wrapMode: Text.WordWrap
                text: root.selectedWindowContainsUs
                    ? qsTr("Lightning runs inside this window's app, so its "
                           + "sound can't be shared on its own without "
                           + "sending the call back. Its sound isn't chosen "
                           + "automatically.")
                    : root.selectedWindowAudioKey.length === 0
                      ? qsTr("Lightning can't tell which app this window "
                             + "belongs to, so its sound isn't chosen "
                             + "automatically. Choose the app below.")
                      : qsTr("This window's app isn't playing any sound right "
                             + "now, so its sound isn't chosen automatically.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }

            ScrollView {
                objectName: "shareAudioAppsScroll"
                Layout.fillWidth: true
                visible: root.canChooseApps && !!app.groupCall
                         && app.groupCall.shareAudioMode === 2
                Layout.preferredHeight: visible
                    ? Math.min(pickerAppList.implicitHeight,
                               AppTheme.scaled(132))
                    : 0
                clip: true
                contentWidth: availableWidth
                ShareAudioAppList {
                    id: pickerAppList
                    width: parent ? parent.width : implicitWidth
                    visible: root.visible && !!app.groupCall
                             && app.groupCall.shareAudioMode === 2
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
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
                    { label: qsTr("Auto"), value: 0 },
                    { label: qsTr("720p"), value: 720 },
                    { label: qsTr("1080p"), value: 1080 },
                    { label: qsTr("1440p"), value: 1440 },
                    { label: qsTr("4K"), value: 2160 }
                ]
                Accessible.name: qsTr("Screen share resolution")
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
                    { label: qsTr("Auto"), value: 0 },
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
                enabled: root.viewIndexOf(root.selected) >= 0
                onClicked: root.confirmShare()
            }
        }
    }
}
