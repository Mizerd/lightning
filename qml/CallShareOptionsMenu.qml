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

    // EXPLICIT WIDTH, like every other menu here that carries a real
    // sentence. AppMenu's own comment says the design width is "a floor, not
    // a clamp" and that rows outgrowing it widen to fit — but its content
    // item is a ListView, which does not measure its delegates' widths, so
    // `implicitContentWidth` never exceeds the floor and the row elides
    // instead. That the four other menus needing more room all set this
    // property by hand is the evidence. Reported as a sound toggle whose
    // label read "Share this computer's ...".
    menuWidth: 260

    // THE MENU STAYS OPEN while these are changed. A MenuItem closes its
    // menu on trigger — correct for an action, wrong for a settings panel
    // where someone reasonably wants to pick a resolution AND a rate. Qt
    // offers no "do not close" on MenuItem, so the close is allowed to
    // happen and the menu is reopened at the same place; `x`/`y` were set by
    // popup() and are not touched, so it returns exactly where it was.
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
        // ABSENT, not disabled, where nothing can capture what the computer
        // plays: a disabled Qt Quick control gets no hover, so it cannot even
        // explain why it is greyed.
        visible: app.groupCall && app.groupCall.shareAudioSupported
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: app.groupCall && app.groupCall.shareAudioEnabled
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
            // The marker rides the ROW it applies to, so there is no
            // paragraph to read and nothing to run off the edge. Asked of
            // SettingsManager rather than re-derived here, so the rule stays
            // in one place.
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
