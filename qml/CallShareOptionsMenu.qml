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

    MenuSectionLabel { text: qsTr("Screen share") }

    AppMenuItem {
        objectName: "shareAudioMenuItem"
        // Absent rather than disabled where nothing can capture system audio: a
        // disabled control gets no hover to explain itself.
        visible: app.groupCall && app.groupCall.shareAudioSupported
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: app.groupCall && app.groupCall.shareAudioEnabled
        // Short label: the label column is 200px
        // (theShareOptionsMenuShowsItsLabelsWithoutEliding). What the capture
        // contains (the whole output mix, this call included) is explained on
        // ScreenSharePicker's "Share audio" checkbox and in
        // docs/voice-calls.md.
        text: qsTr("Share computer sound")
        onTriggered: root.actAndStayOpen(function () {
            if (app.groupCall) {
                app.groupCall.shareAudioEnabled =
                    !app.groupCall.shareAudioEnabled;
            }
        })
    }

    AppMenuSeparator {
        visible: app.groupCall && app.groupCall.shareAudioSupported
        height: visible ? implicitHeight : 0
    }

    MenuSectionLabel { text: qsTr("Resolution") }

    Repeater {
        model: [
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
}
