import QtQuick
import QtQuick.Controls
import MatrixClient

AppMenu {
    id: root

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

    signal chooseAppsRequested()

    readonly property bool _soundSupported:
        !!app.groupCall && app.groupCall.shareAudioSupported
    readonly property bool _soundShown: !!app.groupCall
    onAboutToShow: {
        if (app.groupCall)
            app.groupCall.refreshShareAudioCapability();
    }
    readonly property int _soundMode:
        app.groupCall ? app.groupCall.shareAudioMode : 0

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
        visible: root._soundSupported && !!app.groupCall
                 && app.groupCall.shareAudioCanChooseApps
        height: visible ? implicitHeight : 0
        radio: true
        radioSelected: root._soundMode === 2
        text: qsTr("Choose apps…")
        onTriggered: {
            if (app.groupCall && app.groupCall.shareAudioApps.length > 0)
                app.groupCall.shareAudioMode = 2;
            root.chooseAppsRequested();
        }
    }

    AppMenuItem {
        objectName: "shareAudioMuteItem"
        readonly property bool _muted:
            !!app.groupCall && app.groupCall.shareAudioMuted
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

    Label {
        objectName: "shareAudioStatusLabel"
        visible: root._soundShown
        height: root._soundShown ? implicitHeight : 0
        width: root.width - root.leftPadding - root.rightPadding
        leftPadding: AppTheme.menuItemPadding
        rightPadding: AppTheme.menuItemPadding
        topPadding: AppTheme.spacing4
        bottomPadding: AppTheme.spacing6
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
