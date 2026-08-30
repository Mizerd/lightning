import QtQuick
import QtQuick.Controls
import MatrixClient

// Screen-share options, on the chevron beside the share button.
//
// HERE BECAUSE THE SHARE PICKER IS NOT REACHABLE EVERYWHERE. On Wayland the
// route is LinuxShareRoute::Portal: the desktop draws its own source dialog
// and Lightning's picker never opens, so anything that lives only in that
// picker is reachable from X11, Windows and macOS and invisible on KDE.
// Reported twice — first for the audio switch, then for resolution and rate.
// The call bar is the one surface every route shows.
//
// It also gets the audio toggle off the bar itself, which was a full-width
// control for a setting most people touch once.
AppMenu {
    id: root

    MenuSectionLabel { text: qsTr("Screen share") }

    AppMenuItem {
        objectName: "shareAudioMenuItem"
        // ABSENT, not disabled, where nothing can capture what the computer
        // plays: a disabled Qt Quick control gets no hover, so it cannot even
        // explain why it is greyed.
        visible: app.groupCall && app.groupCall.shareAudioSupported
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: app.groupCall && app.groupCall.shareAudioEnabled
        text: qsTr("Share this computer's sound")
        onTriggered: {
            if (app.groupCall) {
                app.groupCall.shareAudioEnabled =
                    !app.groupCall.shareAudioEnabled;
            }
        }
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
            onTriggered: app.settings.shareMaxHeight = modelData.value
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
            text: modelData.label
            radio: true
            radioSelected: app.settings.shareFps === modelData.value
            onTriggered: app.settings.shareFps = modelData.value
        }
    }
}
