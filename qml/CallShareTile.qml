import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

Item {
    id: root

    readonly property bool softwareRendererHidesVideo:
        typeof app !== "undefined" && app && app.softwareRenderer === true

    /// Stable id of this share for one call: the LiveKit screen-share track
    /// sid, or "local:<n>" for ours. A restarted share is a new track and a new
    /// id, so it can never arrive already dismissed.
    property string shareId: ""
    property string ownerIdentity: ""
    property string ownerDisplayName: ""
    property string trackKey: ""
    property bool local: false
    property string unsupportedCodec: root.local || root.trackKey.length === 0
        ? "" : app.groupCall.unsupportedVideoCodec(root.trackKey)

    property bool compact: false
    property bool focused: false

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    readonly property string _label: root.local
        ? qsTr("Your screen")
        : (root.ownerDisplayName.length > 0
           ? qsTr("%1's screen").arg(root.ownerDisplayName)
           : qsTr("Shared screen"))

    onTrackKeyChanged: if (videoLoader.item)
        videoLoader.item.attach()

    Accessible.role: Accessible.Button
    Accessible.name: root._label
    Accessible.description: qsTr("Screen share")
    Accessible.focusable: true
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusTile
        color: AppTheme.stormInset
        readonly property bool pictureFramed:
            videoLoader.visible && videoLoader.item
            && videoLoader.item.pictureFramed === true
        border.width: root.activeFocus ? 2 : (pictureFramed ? 0 : (root.focused ? 2 : 1))
        border.color: root.activeFocus
                      ? AppTheme.focusRing
                      : (root.focused ? AppTheme.accentBorder
                                      : AppTheme.borderSubtle)
        clip: true

        Loader {
            id: videoLoader
            anchors.fill: parent
            anchors.margins: 1
            active: root.ownerIdentity.length > 0
            visible: active && item && item.hasFrame
                     && !root.softwareRendererHidesVideo
            sourceComponent: Item {
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0
                readonly property bool pictureFramed: videoFrame.visible

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                }
                Rectangle {
                    id: videoFrame
                    objectName: "callShareVideoFrame"
                    readonly property real videoW:
                        output.videoSink ? output.videoSink.videoSize.width : 0
                    readonly property real videoH:
                        output.videoSink ? output.videoSink.videoSize.height : 0
                    readonly property real fit:
                        videoW > 0 && videoH > 0
                        ? Math.min(output.width / videoW, output.height / videoH)
                        : 0
                    readonly property real paintedW: Math.round(videoW * fit)
                    readonly property real paintedH: Math.round(videoH * fit)
                    x: output.x + Math.round((output.width - paintedW) / 2)
                    y: output.y + Math.round((output.height - paintedH) / 2)
                    width: paintedW
                    height: paintedH
                    visible: fit > 0
                    color: "transparent"
                    radius: AppTheme.radiusSm
                    border.width: 2
                    border.color: root.focused ? AppTheme.accentBorder
                                               : Qt.rgba(1, 1, 1, 0.34)
                }

                function attach() {
                    if (root.local)
                        app.groupCall.attachLocalScreenSink(output.videoSink);
                    else
                        app.groupCall.attachScreenSink(root.ownerIdentity,
                                                       output.videoSink);
                }
                function detach() {
                    // Names the sink, never the key: a dying tile's key-named
                    // detach would unhook the surface that just replaced it.
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacing16
            spacing: 6
            visible: !videoLoader.visible
            Icon {
                Layout.alignment: Qt.AlignHCenter
                // A compact tile has room for the reason or the glyph, not both.
                visible: !(root.compact && root.unsupportedCodec.length > 0)
                name: "screen_share"
                size: root.compact ? 20 : 30
                color: AppTheme.stormTextSecondary
            }
            Loader {
                Layout.fillWidth: true
                active: root.unsupportedCodec.length > 0
                        || (!root.compact && root._label.length > 0)
                visible: active
                sourceComponent: Text {
                    objectName: "callShareStatus"
                    textFormat: Text.PlainText
                    text: root.unsupportedCodec.length === 0
                          ? qsTr("Waiting for the picture…")
                          : (root.compact
                             ? qsTr("%1 isn't supported by this build").arg(root.unsupportedCodec)
                             : qsTr("Can't play this stream: %1 isn't supported by this build").arg(root.unsupportedCodec))
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: root.compact ? 11 : 12
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        readonly property bool plateAutoHides: root.width >= 480
        property int plateIdleTicks: 0
        property bool plateIdle: false
        Timer {
            id: plateIdleTimer
            objectName: "callSharePlateIdleTimer"
            interval: 500
            repeat: true
            running: surface.plateAutoHides && !surface.plateIdle
            onTriggered: {
                surface.plateIdleTicks += 1
                if (surface.plateIdleTicks >= 6)
                    surface.plateIdle = true
            }
        }
        onPlateAutoHidesChanged: {
            surface.plateIdleTicks = 0
            surface.plateIdle = false
        }
        property real plateLastX: -1
        property real plateLastY: -1
        HoverHandler {
            id: plateHover
            enabled: surface.plateAutoHides
            onPointChanged: {
                const p = plateHover.point.position
                if (Math.abs(p.x - surface.plateLastX) < 1
                    && Math.abs(p.y - surface.plateLastY) < 1)
                    return
                surface.plateLastX = p.x
                surface.plateLastY = p.y
                surface.plateIdleTicks = 0
                surface.plateIdle = false
            }
        }
        Rectangle {
            id: namePlate
            objectName: "callShareNameplate"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 8
            opacity: surface.plateAutoHides && surface.plateIdle ? 0 : 1
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 220 } }
            implicitWidth: Math.min(plate.implicitWidth + 12,
                                    surface.width - (root.compact ? 12 : 16))
            implicitHeight: plate.implicitHeight + 6
            radius: AppTheme.radiusPill
            color: Qt.rgba(0, 0, 0, 0.55)

            RowLayout {
                id: plate
                objectName: "callShareNameplateRow"
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4
                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    name: "screen_share"
                    size: root.compact ? 12 : 14
                    color: AppTheme.success
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root._label
                    color: "#FFFFFF"
                    font.pixelSize: root.compact ? 11 : 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }

        TapHandler {
            // Left button only: TapHandlers are non-exclusive across subtrees,
            // so taking every button would swallow presses meant for the stage.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        HoverHandler {
            id: shareHover
            cursorShape: Qt.PointingHandCursor
        }
    }
    property int shareAudioRevision: 0
    Connections {
        target: app.groupCall
        function onParticipantsChanged() {
            root.shareAudioRevision = root.shareAudioRevision + 1
        }
    }
    readonly property bool shareAudioOffered: {
        var _ = root.shareAudioRevision
        return root.shareId.length > 0 && app.groupCall
            && app.groupCall.shareHasAudio(root.shareId)
    }

    property int shareVolumeRevision: 0
    function currentShareVolume() {
        var _ = root.shareVolumeRevision
        if (!root.shareAudioOffered)
            return 100
        var v = app.groupCall.shareVolume(root.shareId)
        return (v === undefined || v === null) ? 100 : v
    }
    function applyShareVolume(percent) {
        if (!root.shareAudioOffered)
            return
        app.groupCall.setShareVolume(root.shareId, Math.round(percent))
        root.shareVolumeRevision = root.shareVolumeRevision + 1
    }
    function openShareVolume() {
        if (root.shareAudioOffered)
            shareVolumePopup.open()
    }

    TapHandler {
        enabled: root.shareAudioOffered
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onTapped: root.openShareVolume()
    }

    Popup {
        id: shareVolumePopup
        objectName: "callShareVolumePopup"
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 268
        margins: 8
        padding: AppTheme.spacing12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        onOpened: shareVolumeSlider.value = root.currentShareVolume()

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: AppTheme.stormPanel
            border.width: 1
            border.color: AppTheme.stormBorder
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
            }
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Icon {
                    name: shareVolumeSlider.value > 0 ? "volume_up"
                                                      : "volume_off"
                    size: 18
                    color: AppTheme.stormTextSecondary
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Screen share")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    objectName: "callShareVolumeReadout"
                    text: Math.round(shareVolumeSlider.value) + "%"
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                }
            }
            Slider {
                id: shareVolumeSlider
                objectName: "callShareVolumeSlider"
                Layout.fillWidth: true
                from: 0
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Screen share volume")
                onMoved: root.applyShareVolume(value)
                onPressedChanged: if (app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(pressed)
                Component.onDestruction: if (pressed
                        && app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(false)
                background: Rectangle {
                    x: shareVolumeSlider.leftPadding
                    y: shareVolumeSlider.topPadding
                       + shareVolumeSlider.availableHeight / 2 - 2
                    width: shareVolumeSlider.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.stormInset
                    Rectangle {
                        width: shareVolumeSlider.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.bolt
                    }
                    Rectangle {
                        x: Math.round(parent.width / 2) - 1
                        y: -3
                        width: 2
                        height: parent.height + 6
                        radius: 1
                        color: AppTheme.stormTextMuted
                    }
                }
                handle: Rectangle {
                    x: shareVolumeSlider.leftPadding
                       + shareVolumeSlider.visualPosition
                         * (shareVolumeSlider.availableWidth - width)
                    y: shareVolumeSlider.topPadding
                       + shareVolumeSlider.availableHeight / 2 - height / 2
                    width: 16
                    height: 16
                    radius: 8
                    color: AppTheme.bolt
                    border.width: 1
                    border.color: AppTheme.stormBorder
                }
            }
        }
    }

}
