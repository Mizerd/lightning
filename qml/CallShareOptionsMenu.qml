import QtQuick
import QtQuick.Controls
import MatrixClient

// Screen-share options, on the chevron beside the share button. On Wayland
// (LinuxShareRoute::Portal) the desktop draws its own source dialog and
// Lightning's picker never opens, so options must also live on the call bar,
// the one surface every route shows.
AppMenu {
    id: root

    // Explicit width, as in the other menus whose labels are full phrases, so
    // labels don't elide.
    menuWidth: 260

    // Keeps the menu open while options change: a MenuItem always closes its
    // menu, so it is reopened at the same place (popup() set x/y, which are
    // left untouched).
    property bool keepOpen: false
    function actAndStayOpen(fn) {
        root.keepOpen = true;
        fn();
    }
    onClosed: {
        if (root.keepOpen) {
            root.keepOpen = false;
            root.open();
        }
    }

    /// "Choose apps…" was picked: the host opens ShareAudioAppsDialog (a
    /// dialog cannot live inside a Menu, whose children become rows).
    signal chooseAppsRequested()

    readonly property bool _soundSupported:
        !!app.groupCall && app.groupCall.shareAudioSupported
    // The section stays while a call exists, even where nothing can capture:
    // the line at its foot then says why, and the two choices that remain are
    // disabled. Absent would leave a user with no sound and no reason.
    readonly property bool _soundShown: !!app.groupCall
    // An earlier probe may have been inconclusive (the audio daemon was not
    // up yet): ask again as the menu opens. Bounded in the controller.
    onAboutToShow: {
        if (app.groupCall)
            app.groupCall.refreshShareAudioCapability();
    }
    readonly property int _soundMode:
        app.groupCall ? app.groupCall.shareAudioMode : 0

    // ── Sound ──
    // Three choices, not a toggle: no sound, the whole system (Lightning is
    // always left out; share sound is off where it cannot be), or only chosen
    // apps. Where nothing can capture, "No sound" and "Entire system" stay
    // but are disabled, and the status line below gives the reason in words
    // (a disabled control gets no hover to explain itself). Short labels: the
    // label column is 200px (theShareOptionsMenuShowsItsLabelsWithoutEliding).
    MenuSectionLabel {
        visible: root._soundShown
        height: root._soundShown ? implicitHeight : 0
        text: qsTr("Screen share sound")
    }

    AppMenuItem {
        objectName: "shareAudioOffItem"
        visible: root._soundShown
        enabled: root._soundSupported
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: root._soundMode === 0
        text: qsTr("No sound")
        onTriggered: root.actAndStayOpen(function () {
            if (app.groupCall)
                app.groupCall.shareAudioMode = 0;
        })
    }

    AppMenuItem {
        objectName: "shareAudioMenuItem"
        visible: root._soundShown
        enabled: root._soundSupported
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: root._soundMode === 1
        text: qsTr("Entire system")
        onTriggered: root.actAndStayOpen(function () {
            if (app.groupCall)
                app.groupCall.shareAudioMode = 1;
        })
    }

    AppMenuItem {
        objectName: "shareAudioAppsItem"
        // Only where single applications can be captured; elsewhere the
        // status line says why not.
        visible: root._soundSupported && !!app.groupCall
                 && app.groupCall.shareAudioCanChooseApps
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: root._soundMode === 2
        text: qsTr("Choose apps…")
        onTriggered: {
            // A choice made earlier applies at once; with none, the mode
            // changes on the first tick in the dialog.
            if (app.groupCall && app.groupCall.shareAudioApps.length > 0)
                app.groupCall.shareAudioMode = 2;
            root.chooseAppsRequested();
        }
    }

    // Mute the running share's sound alone, never the microphone. Only while a
    // share is carrying sound: before one there is nothing to mute, and the
    // three choices above already say "No sound". Lasts for this share.
    AppMenuItem {
        objectName: "shareAudioMuteItem"
        readonly property bool _muted:
            !!app.groupCall && app.groupCall.shareAudioMuted
        // Applications mode with none chosen carries no sound: nothing to
        // mute (the status line already says so).
        visible: root._soundSupported && !!app.groupCall
                 && app.groupCall.screenSharing && root._soundMode !== 0
                 && !(root._soundMode === 2
                      && app.groupCall.shareAudioApps.length === 0)
        height: visible ? implicitHeight : 0
        iconName: _muted ? "volume_off" : "volume_up"
        text: _muted ? qsTr("Unmute share sound") : qsTr("Mute share sound")
        onTriggered: root.actAndStayOpen(function () {
            if (app.groupCall)
                app.groupCall.toggleShareAudioMuted();
        })
    }

    // What the sound carries, in words, including "nothing is being heard"
    // during a share. Not a row: sized to the menu so it wraps, and it does
    // not vote in AppMenu's width fit.
    Label {
        objectName: "shareAudioStatusLabel"
        visible: root._soundShown
        height: root._soundShown ? implicitHeight : 0
        width: root.width - root.leftPadding - root.rightPadding
        leftPadding: AppTheme.menuItemPadding
        rightPadding: AppTheme.menuItemPadding
        topPadding: AppTheme.spacing4
        bottomPadding: AppTheme.spacing6
        // Application names reach this sentence: never markup.
        textFormat: Text.PlainText
        text: app.groupCall ? app.groupCall.shareAudioStatus : ""
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        wrapMode: Text.WordWrap
    }

    AppMenuSeparator {
        visible: root._soundShown
        height: visible ? implicitHeight : 0
    }

    MenuSectionLabel { text: qsTr("Resolution") }

    Repeater {
        model: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("720p"), value: 720 },
            { label: qsTr("1080p"), value: 1080 },
            { label: qsTr("1440p"), value: 1440 },
            { label: qsTr("4K"), value: 2160 }
        ]
        delegate: AppMenuItem {
            required property var modelData
            text: modelData.label
            radio: true
            radioSelected: app.settings.shareMaxHeight === modelData.value
            onTriggered: root.actAndStayOpen(function () {
                app.settings.shareMaxHeight = modelData.value;
            })
        }
    }

    AppMenuSeparator {}

    MenuSectionLabel { text: qsTr("Frame rate") }



    Repeater {
        model: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("15 fps"), value: 15 },
            { label: qsTr("30 fps"), value: 30 },
            { label: qsTr("60 fps"), value: 60 }
        ]
        delegate: AppMenuItem {
            required property var modelData
            // The marker sits on the row it applies to. The rule comes from
            // SettingsManager, not re-derived here.
            readonly property bool slow:
                app.settings.shareQualityDemandingAt(
                    app.settings.shareMaxHeight, modelData.value)
            text: slow ? qsTr("%1 — slow").arg(modelData.label)
                       : modelData.label
            radio: true
            radioSelected: app.settings.shareFps === modelData.value
            onTriggered: root.actAndStayOpen(function () {
                app.settings.shareFps = modelData.value;
            })
        }
    }

    AppMenuSeparator {}

    MenuSectionLabel { text: qsTr("Bitrate limit") }

    Repeater {
        objectName: "shareBitrateRows"
        model: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("0.5 Mbps"), value: 500 },
            { label: qsTr("1 Mbps"), value: 1000 },
            { label: qsTr("2 Mbps"), value: 2000 },
            { label: qsTr("3 Mbps"), value: 3000 },
            { label: qsTr("4.5 Mbps"), value: 4500 }
        ]
        delegate: AppMenuItem {
            required property var modelData
            text: modelData.label
            radio: true
            radioSelected:
                app.settings.shareBitrateCapKbps === modelData.value
            onTriggered: root.actAndStayOpen(function () {
                app.settings.shareBitrateCapKbps = modelData.value;
            })
        }
    }
}
