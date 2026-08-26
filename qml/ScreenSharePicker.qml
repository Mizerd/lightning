import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Choose which display to share, on the platforms that have no portal.
//
// WHY THIS EXISTS ONLY OFF LINUX. The xdg desktop portal owns the picker
// there: it shows its own dialog, the user chooses in it, and Lightning
// receives a PipeWire node for exactly what was chosen — which is what makes
// sharing safe on Wayland, and why ScreenCastPortal never enumerates anything
// itself. Windows and macOS have no such broker, so the capture element takes
// a display index and nothing was asking the user which one: a share silently
// took whichever display the app happened to be on.
//
// `app.groupCall.screenShareSources` is EMPTY on Linux, always, so this never
// opens there — two dialogs for one gesture would be worse than none.
//
// DISPLAYS ONLY, and the wording says so rather than implying more. The
// capture elements that can be shipped capture a monitor (`gdiscreencapsrc`)
// or a display (`avfvideosrc capture-screen`); single-window capture needs
// elements this toolchain cannot build. Offering a window list the pipeline
// would then fail on is worse than not offering one.
AppDialog {
    id: root

    title: qsTr("Choose what to share")
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(520, parent ? parent.width - 64 : 520)

    /// The rows to offer. Bound to the live call, and NOT readonly — the
    /// binding is the production path, and a test that cannot put rows in it
    /// cannot press one. That gap is not theoretical: the grouped-list rework
    /// shipped with its rows unclickable, and every check that existed passed
    /// because none of them had a row to click.
    property var sources: app.groupCall ? app.groupCall.screenShareSources : []
    property int selected: 0

    // Split for the two group headers. A row is a WINDOW when it carries a
    // non-zero handle — the same fact the controller keys the capture off, so
    // the list and the pipeline cannot disagree about what a row means.
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
            root.open();
        }
    }
    // The controller clears the list when the share starts or is abandoned,
    // so a dialog left open by any other path closes with it rather than
    // sitting over a call it can no longer act on.
    onSourcesChanged: {
        if (root.visible && root.sources.length === 0)
            root.close();
    }
    /// Set for the one frame between pressing Share and the controller
    /// clearing the list.
    ///
    /// Without it `onClosed` fires while the sources are still populated,
    /// cancels the selection, and the `chooseScreenShareSource` call that
    /// follows finds an empty list and returns — a Share button that closes
    /// the dialog and shares nothing.
    property bool accepting: false

    onRejected: app.groupCall.cancelScreenShareSelection()
    onClosed: {
        if (!root.accepting && root.sources.length > 0)
            app.groupCall.cancelScreenShareSelection();
        root.accepting = false;
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            // Says what is shared AND what is not, because a person who came
            // here looking for "share one window" should find that out now
            // rather than after everyone has seen their whole desktop.
            // Says what each choice actually shares, because "share my
            // screen" and "share this window" have different consequences
            // and the difference is the whole reason to offer both.
            text: root.screenCount === root.sources.length
                ? qsTr("Everyone in the call sees the whole display you pick.")
                : qsTr("A screen shares everything on it. A window shares only that window, even if something is in front of it.")
        }

        ListView {
            id: list
            // Named so a test can find the list and press a real row; the
            // grouped rework shipped with its rows unclickable precisely
            // because nothing could reach one.
            objectName: "sourceList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(320, contentHeight)
            clip: true
            model: root.sources
            spacing: 2
            ScrollBar.vertical: AppScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: Column {
                id: rowColumn
                required property var modelData
                required property int index
                width: list.width

                // The header belongs to the FIRST row of each kind rather
                // than to a section delegate: the model is a plain
                // QVariantList with no section role, and giving the rows
                // themselves the header keeps one source of truth for the
                // ordering.
                readonly property bool startsWindows:
                    root.isWindowRow(modelData)
                    && index === root.screenCount
                readonly property bool startsScreens:
                    !root.isWindowRow(modelData) && index === 0

                Loader {
                    active: rowColumn.startsScreens || rowColumn.startsWindows
                    sourceComponent: Label {
                        topPadding: rowColumn.startsWindows ? AppTheme.spacing12 : 0
                        bottomPadding: 4
                        leftPadding: AppTheme.spacing12
                        text: rowColumn.startsWindows ? qsTr("Windows")
                                                      : qsTr("Screens")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        font.weight: AppTheme.weightMedium
                    }
                }

            ItemDelegate {
                readonly property var modelData: rowColumn.modelData
                readonly property int index: rowColumn.index
                width: list.width
                // Tall enough for the 36px preview box plus breathing room.
                height: 56
                padding: 0
                hoverEnabled: true
                onClicked: root.selected = index
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name + " " + modelData.geometry

                background: Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: AppTheme.radiusMd
                    color: root.selected === index ? AppTheme.selected
                                                   : (parent.hovered ? AppTheme.hover : "transparent")
                    border.width: root.selected === index ? 2 : 1
                    border.color: root.selected === index ? AppTheme.accent : AppTheme.border
                }

                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing12
                    anchors.rightMargin: AppTheme.spacing12
                    spacing: AppTheme.spacing12

                    // A LIVE PREVIEW of what this row would share, with the
                    // glyph behind it as the fallback.
                    //
                    // Both are always present and the picture simply covers
                    // the glyph when it loads: a Loader swap would make the
                    // row change height as thumbnails arrive one by one, and
                    // a list that reflows while you are aiming at it is worse
                    // than a plain one. Fixed box, so the rows stay aligned
                    // whatever the aspect ratio.
                    Item {
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 36

                        Icon {
                            anchors.centerIn: parent
                            // BOTH names checked against Icon.qml's map. The
                            // bundled font is a SUBSET and an unmapped name
                            // renders as tofu; `web_asset`, the obvious glyph
                            // for a window, is not in it.
                            name: root.isWindowRow(modelData) ? "fit_screen"
                                                              : "screen_share"
                            size: 22
                            color: root.selected === index ? AppTheme.accent
                                                           : AppTheme.textSecondary
                        }

                        Image {
                            id: preview
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false   // a live grab; a cached one is a lie
                            // Bounded on the way in as well as in the
                            // provider: a tile is 64px, and asking for the
                            // desktop at full resolution to draw it would
                            // cost megabytes per row.
                            sourceSize.width: 128
                            // The id carries WHICH thing, in the same terms
                            // the controller uses to start the capture — a
                            // window handle or the row's display index — so a
                            // tile cannot preview something other than what
                            // pressing Share would send.
                            source: root.isWindowRow(modelData)
                                ? "image://lightning-sharesource/w"
                                  + modelData.windowHandle
                                : "image://lightning-sharesource/s"
                                  + (modelData.index !== undefined
                                     ? modelData.index : 0)
                            // Null image (a closed window, or any platform
                            // without previews) leaves the glyph showing.
                            visible: status === Image.Ready
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            elide: Label.ElideRight
                            text: modelData.name
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            font.weight: AppTheme.weightMedium
                        }
                        Label {
                            Layout.fillWidth: true
                            elide: Label.ElideRight
                            text: modelData.geometry
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        }
                    }
                    // Two different facts, and neither is decoration: which
                    // display the OS calls primary, and which one this window
                    // is on — the second is what the user most likely means.
                    StatusChip {
                        visible: modelData.current === true
                        tone: "accent"
                        label: qsTr("This screen")
                    }
                    StatusChip {
                        visible: modelData.primary === true && modelData.current !== true
                        tone: "info"
                        label: qsTr("Primary")
                    }
                }
            }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            AppButton {
                kind: "primary"
                text: qsTr("Share")
                enabled: root.sources.length > 0
                onClicked: {
                    var chosen = root.selected;
                    root.accepting = true;
                    root.close();
                    app.groupCall.chooseScreenShareSource(chosen);
                }
            }
        }
    }
}
